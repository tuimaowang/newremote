#include "message_client.h"
#include "protocol.h"
#include <QHostAddress>

// 初始化 WebSocket、重连与聊天室确认超时处理。
MessageClient::MessageClient(QObject* parent) : QObject(parent)
{
    clock_.start();
    socket_.setMaxAllowedIncomingMessageSize(protocol::maxWireBytes);
    socket_.setMaxAllowedIncomingFrameSize(protocol::maxWireBytes);
    connect(&socket_, &QWebSocket::aboutToClose, this, [this, aborting = false]() mutable {
        if (aborting) return;
        // Abort fatal frame errors before Qt resumes parsing buffered, rejected data.
        const auto code = socket_.closeCode();
        if (code == QWebSocketProtocol::CloseCodeProtocolError
            || code == QWebSocketProtocol::CloseCodeTooMuchData
            || code == QWebSocketProtocol::CloseCodeWrongDatatype) {
            wanted_ = false;
            aborting = true;
            socket_.abort();
            aborting = false;
            emit problem(QStringLiteral("服务器数据违反协议，连接已终止"));
        }
    });
    retry_.setSingleShot(true);
    deadline_.setSingleShot(true);
    maintenance_.setInterval(5000);
    connect(&retry_, &QTimer::timeout, this, &MessageClient::open);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        emit problem(QStringLiteral("连接或认证超时"));
        socket_.abort();
        if (wanted_ && !retry_.isActive()) retry_.start(retryDelay_);
    });
    connect(&socket_, &QWebSocket::connected, this, [this] {
        emit stateChanged(QStringLiteral("正在认证"), false);
        auto request = protocol::message("auth.login",
            {{"device_id", profile_.deviceId}, {"token", profile_.token}});
        authRequest_ = request.value("id").toString();
        send(request);
    });
    connect(&socket_, &QWebSocket::textMessageReceived, this, &MessageClient::receive);
    connect(&socket_, &QWebSocket::errorOccurred, this, [this] {
        emit problem(socket_.errorString());
        if (wanted_ && socket_.state() == QAbstractSocket::UnconnectedState && !retry_.isActive()) {
            deadline_.stop();
            emit stateChanged(QStringLiteral("连接失败，等待重连"), false);
            retry_.start(retryDelay_);
            retryDelay_ = qMin(retryDelay_ * 2, 15000);
        }
    });
    connect(&socket_, &QWebSocket::sslErrors, this, [this] {
        wanted_ = false;
        retry_.stop();
        deadline_.stop();
        emit problem(QStringLiteral("TLS 证书验证失败；未忽略证书错误"));
        socket_.abort();
    });
    connect(&socket_, &QWebSocket::disconnected, this, [this] {
        online_ = false;
        deadline_.stop();
        maintenance_.stop();
        losePending();
        emit devicesChanged({});
        emit stateChanged(wanted_ ? QStringLiteral("连接中断，等待重连") : QStringLiteral("未连接"), false);
        if (wanted_ && !retry_.isActive()) {
            retry_.start(retryDelay_);
            retryDelay_ = qMin(retryDelay_ * 2, 15000);
        }
    });
    connect(&socket_, &QWebSocket::pong, this, [this] { lastPong_ = clock_.elapsed(); });
    connect(&maintenance_, &QTimer::timeout, this, [this] {
        const auto now = clock_.elapsed();
        if (now - lastPong_ > 20000) { socket_.abort(); return; }
        socket_.ping();
        const auto ids = pending_.keys();
        for (const auto& id : ids) {
            if (now - pending_.value(id) >= 10000) {
                pending_.remove(id);
                emit deliveryChanged(id, QStringLiteral("结果未知"));
            }
        }
        const auto chatIds = liaoTianDaifa_.keys(); // 定期检查聊天室未确认请求。
        for (const auto& id : chatIds) { // 避免发送状态一直等待。
            if (now - liaoTianDaifa_.value(id) >= 10000) { // 超过十秒不猜测投递结果。
                liaoTianDaifa_.remove(id); // 清理已超时请求。
                emit deliveryChanged(id, QStringLiteral("结果未知")); // 广播可能成功但响应丢失。
            }
        }
    });
}

MessageClient::~MessageClient()
{
    socket_.disconnect(this);
    socket_.abort();
}

QString MessageClient::validate(const ClientProfile& profile)
{
    const auto& url = profile.server;
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
        || url.hasQuery() || url.hasFragment() || (!url.path().isEmpty() && url.path() != "/")
        || (url.scheme() != "ws" && url.scheme() != "wss"))
        return QStringLiteral("服务器地址必须是 ws:// 或 wss://，不包含账号、路径或查询参数");
    if (url.scheme() == "ws" && !profile.allowInsecureLan
        && !QHostAddress(url.host()).isLoopback()
        && url.host().compare("localhost", Qt::CaseInsensitive) != 0)
        return QStringLiteral("非本机连接必须使用 WSS");
    if (!protocol::validId(profile.deviceId)) return QStringLiteral("设备 ID 只能包含字母、数字、下划线和连字符，最长 80 位");
    if (profile.token.size() < 32 || profile.token.size() > 256)
        return QStringLiteral("访问令牌长度应为 32 到 256 位");
    return {};
}

void MessageClient::start(const ClientProfile& profile)
{
    stop();
    const auto error = validate(profile);
    if (!error.isEmpty()) { emit problem(error); return; }
    profile_ = profile;
    wanted_ = true;
    retryDelay_ = 1000;
    open();
}

void MessageClient::open()
{
    if (!wanted_) return;
    emit stateChanged(QStringLiteral("正在连接"), false);
    deadline_.start(8000);
    socket_.open(profile_.server);
}

void MessageClient::stop()
{
    wanted_ = false;
    retry_.stop();
    deadline_.stop();
    maintenance_.stop();
    socket_.abort();
    online_ = false;
    losePending();
    emit devicesChanged({});
    emit stateChanged(QStringLiteral("未连接"), false);
}

// 连接中断时将私聊和聊天室待确认发送统一标为结果未知。
void MessageClient::losePending()
{
    const auto ids = pending_.keys();
    pending_.clear();
    for (const auto& id : ids) emit deliveryChanged(id, QStringLiteral("结果未知"));
    const auto chatIds = liaoTianDaifa_.keys(); // 收集聊天室未完成请求。
    liaoTianDaifa_.clear(); // 断线不自动重发，以免重复展示。
    for (const auto& id : chatIds) emit deliveryChanged(id, QStringLiteral("结果未知")); // 明示不确定状态。
    zuiHouXuhao_ = 0; // 新连接重新接收服务器房间顺序。
}

bool MessageClient::send(const QJsonObject& message)
{
    if (socket_.state() != QAbstractSocket::ConnectedState
        || socket_.bytesToWrite() > protocol::maxQueuedBytes) return false;
    return socket_.sendTextMessage(protocol::encode(message)) >= 0;
}

QString MessageClient::sendMessage(const QString& target, const QString& text)
{
    if (!online_ || pending_.size() >= 16 || !protocol::validId(target)
        || target == profile_.deviceId || text.trimmed().isEmpty()
        || text.size() > protocol::maxTextLength) return {};
    auto request = protocol::message("message.send", {{"text", text}});
    request.insert("to", target);
    const auto id = request.value("id").toString();
    pending_.insert(id, clock_.elapsed());
    if (!send(request)) { pending_.remove(id); return {}; }
    return id;
}

// 向公共聊天室发送纯文本；成功返回请求 ID，失败返回空串。
QString MessageClient::faSongLiaoTian(const QString& text)
{
    if (!online_ || liaoTianDaifa_.size() >= 16 || text.trimmed().isEmpty()
        || text.size() > protocol::maxTextLength) return {}; // 限制本机待确认数量及正文长度。
    const auto request = protocol::message(QStringLiteral("chat.send"), {{"text", text}}); // 创建唯一请求。
    const auto id = request.value("id").toString(); // 保存响应关联标识。
    liaoTianDaifa_.insert(id, clock_.elapsed()); // 在网络写入前登记。
    if (!send(request)) { liaoTianDaifa_.remove(id); return {}; } // 失败则回滚登记。
    return id; // UI 根据 ID 更新状态。
}

// 解码服务器消息，并按广播序号去重、更新聊天室发送状态。
void MessageClient::receive(const QString& text)
{
    QJsonObject message;
    if (!protocol::decode(text, &message)) {
        emit problem(QStringLiteral("服务器返回无效消息"));
        wanted_ = false;
        socket_.abort();
        return;
    }
    const auto type = message.value("type").toString();
    const auto payload = message.value("payload").toObject();
    const auto replyId = message.value("reply_to").toString();
    if (type == "auth.result" && replyId == authRequest_
        && payload.value("device_id").toString() == profile_.deviceId && !online_) {
        online_ = true;
        deadline_.stop();
        retryDelay_ = 1000;
        lastPong_ = clock_.elapsed();
        maintenance_.start();
        emit stateChanged(QStringLiteral("已连接"), true);
    } else if (type == "error") {
        const auto code = payload.value("code").toString();
        if (replyId == authRequest_) {
            wanted_ = false;
            retry_.stop();
            socket_.abort();
            emit problem(code == "already_online" ? QStringLiteral("该设备已在其他客户端上线")
                                                  : QStringLiteral("设备 ID 或访问令牌不正确"));
        } else if (pending_.remove(replyId) || liaoTianDaifa_.remove(replyId)) { // 两类发送错误均结算。
            const auto status = code == "delivery_unknown" ? QStringLiteral("结果未知")
                : code == "target_offline" ? QStringLiteral("目标已离线")
                : QStringLiteral("发送失败：%1").arg(code);
            emit deliveryChanged(replyId, status);
        }
    } else if (online_ && type == "device.list") {
        emit devicesChanged(payload.value("devices").toArray());
    } else if (online_ && type == "chat.deliver") { // 接收服务器统一广播。
        const auto id = message.value("request_id").toString(); // 自己发送时匹配本地条目。
        const auto from = message.value("from").toString(); // 使用服务器绑定的设备身份。
        const auto body = payload.value("text").toString(); // 正文按纯文本处理。
        const auto sequence = payload.value("sequence").toVariant().toLongLong(); // 读取房间序号。
        const auto at = QDateTime::fromString(message.value("at").toString(), Qt::ISODateWithMs); // 解析服务器时间。
        if (!protocol::validId(from) || body.trimmed().isEmpty() || body.size() > protocol::maxTextLength
            || sequence <= zuiHouXuhao_ || !at.isValid()) return; // 拒绝无效、重复和倒序事件。
        zuiHouXuhao_ = sequence; // 更新本连接的已显示序号。
        emit liaoTianDaoDa(id, from, body, sequence, at); // 通知界面渲染。
    } else if (online_ && type == "chat.accepted" && liaoTianDaifa_.remove(replyId)) { // 服务器已广播。
        emit deliveryChanged(replyId, QStringLiteral("服务器已广播")); // 不表示其他用户已阅读。
    } else if (online_ && type == "message.deliver") {
        const auto id = message.value("id").toString();
        const auto from = message.value("from").toString();
        const auto body = payload.value("text").toString();
        if (!protocol::validId(from) || message.value("to").toString() != profile_.deviceId
            || body.size() > protocol::maxTextLength || body.trimmed().isEmpty()) return;
        emit incoming(id, from, body);
        send(protocol::message("message.received", {{"message_id", id}}));
    } else if (online_ && type == "message.accepted" && pending_.contains(replyId)) {
        emit deliveryChanged(replyId, QStringLiteral("服务器已受理"));
    } else if (online_ && type == "message.received" && pending_.remove(replyId)) {
        emit deliveryChanged(replyId, QStringLiteral("目标已收到"));
    }
}
