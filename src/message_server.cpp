#include "message_server.h"
#include "protocol.h"
#include <QDateTime>
#include <QWebSocket>

MessageServer::MessageServer(QList<DeviceCredential> credentials, QObject* parent)
    : QObject(parent), credentials_(std::move(credentials))
{
    clock_.start();
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &MessageServer::tick);
}

MessageServer::~MessageServer() { stop(); }

// 启动监听；开放模式允许没有预设设备，但必须先成功加载永久注册记录。
bool MessageServer::listen(const QHostAddress& address, quint16 port,
                           const QSslConfiguration* tls, bool allowInsecureLan)
{
    stop();
    error_.clear();
    if (!tls && !address.isLoopback() && !allowInsecureLan) {
        error_ = QStringLiteral("明文 WebSocket 仅允许回环地址；如需局域网访问，请配置 TLS。"); // 说明明文连接的安全限制和解决方式。
        return false;
    }
    if (!zhuCeBiao_.kaiFang() && !zhuCeBiao_.duQu(credentials_, {}, &error_)) return false; // 固定模式也通过注册模块校验全部预设凭据。
    if (!zhuCeBiao_.kaiFang() && (credentials_.isEmpty() || credentials_.size() > 32)) { // 只对旧固定名单保留原数量要求。
        error_ = QStringLiteral("固定名单模式的设备数量必须在 1 到 32 台之间。"); // 动态注册不受累计 32 台限制。
        return false; // 空名单的固定模式没有任何可登录身份。
    }
    server_ = std::make_unique<QWebSocketServer>(QStringLiteral("FSRemote Messages"),
        tls ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode);
    if (tls) server_->setSslConfiguration(*tls);
    server_->setHandshakeTimeout(5000);
    server_->setMaxPendingConnections(32);
    connect(server_.get(), &QWebSocketServer::newConnection, this, &MessageServer::accept);
    if (!server_->listen(address, port)) {
        error_ = server_->errorString();
        return false;
    }
    timer_.start();
    return true;
}

// 在监听前加载身份库；注册文件损坏或不可读时明确拒绝启用。
bool MessageServer::qiYongZhuCe(const QString& luJing)
{
    if (server_) { // 运行中不能用另一份注册表替换在线身份。
        error_ = QStringLiteral("请在启动监听之前设置设备注册记录。"); // 明确说明调用顺序要求。
        return false; // 保留当前在线会话和身份库。
    }
    return zhuCeBiao_.duQu(credentials_, luJing, &error_); // 注册模块负责完整校验并恢复动态设备。
}

quint16 MessageServer::port() const { return server_ ? server_->serverPort() : 0; }
QString MessageServer::errorString() const { return error_; }

// 停止监听并清空当前连接、投递与聊天室待处理状态。
void MessageServer::stop()
{
    timer_.stop();
    if (server_) server_->close();
    const auto sockets = peers_.keys();
    for (auto* socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    peers_.clear();
    online_.clear();
    pending_.clear();
    liaoTianDuilie_.clear(); // 停服时丢弃尚未广播的临时消息。
    liaoTianYipaicheng_ = false; // 下次启动允许重新安排分发。
    server_.reset();
}

void MessageServer::accept()
{
    while (server_->hasPendingConnections()) {
        auto* socket = server_->nextPendingConnection();
        socket->setParent(this);
        if (peers_.size() >= 32) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("capacity"));
            socket->deleteLater();
            continue;
        }
        socket->setMaxAllowedIncomingMessageSize(protocol::maxWireBytes);
        socket->setMaxAllowedIncomingFrameSize(protocol::maxWireBytes);
        connect(socket, &QWebSocket::aboutToClose, socket, [socket, aborting = false]() mutable {
            if (aborting) return;
            // Stop parsing rejected frames immediately; Qt 6.11 may otherwise queue repeated close timers.
            const auto code = socket->closeCode();
            if (code == QWebSocketProtocol::CloseCodeProtocolError
                || code == QWebSocketProtocol::CloseCodeTooMuchData
                || code == QWebSocketProtocol::CloseCodeWrongDatatype) {
                aborting = true;
                socket->abort();
                aborting = false;
            }
        });
        Peer peer;
        peer.connectedAt = peer.lastPong = peer.rateStart = clock_.elapsed();
        peers_.insert(socket, peer);
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString& text) { receive(socket, text); });
        connect(socket, &QWebSocket::binaryMessageReceived, this, [socket] {
            socket->close(QWebSocketProtocol::CloseCodeDatatypeNotSupported);
        });
        connect(socket, &QWebSocket::errorOccurred, this, [socket] {
            socket->abort();
        });
        connect(socket, &QWebSocket::pong, this, [this, socket] {
            if (peers_.contains(socket)) peers_[socket].lastPong = clock_.elapsed();
        });
        connect(socket, &QWebSocket::disconnected, this, [this, socket] { disconnected(socket); });
    }
}

bool MessageServer::send(QWebSocket* socket, const QJsonObject& message)
{
    if (!peers_.contains(socket) || socket->state() != QAbstractSocket::ConnectedState) return false;
    if (socket->bytesToWrite() > protocol::maxQueuedBytes) {
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("backpressure"));
        return false;
    }
    return socket->sendTextMessage(protocol::encode(message)) >= 0;
}

void MessageServer::reply(QWebSocket* socket, const QString& type, const QString& requestId,
                          QJsonObject payload)
{
    auto response = protocol::message(type, payload);
    response.insert("reply_to", requestId);
    send(socket, response);
}

void MessageServer::fail(QWebSocket* socket, const QString& requestId, const QString& code)
{
    reply(socket, QStringLiteral("error"), requestId, {{"code", code}});
}

// 生成成员列表：在线设备优先，最多 32 条；特殊名称按序列化字节限制显示长度，确保客户端能够接收。
QJsonArray MessageServer::roster() const
{
    QJsonArray devices;
    QSet<QString> xianShi; // 大量历史注册记录不全部广播，避免成员消息超过协议上限。
    for (auto it = online_.cbegin(); it != online_.cend(); ++it) xianShi.insert(it.key()); // 最多 32 个在线身份必须全部展示。
    for (const auto& credential : credentials_) { // 兼容原先固定名单的离线成员展示。
        if (xianShi.size() >= 32) break; // 单次成员广播最多 32 条。
        xianShi.insert(credential.id); // 未在线的预设身份填补剩余位置。
    }
    for (const auto& credential : zhuCeBiao_.sheBei()) { // 保留登记顺序，旧客户端列表行为仍然稳定。
        if (!xianShi.contains(credential.id)) continue; // 自动注册的历史离线身份不占成员消息空间。
        QJsonObject chengYuan{{"id", credential.id}, {"name", credential.name}, // 身份完整保留，名称通常也完整显示。
                             {"online", online_.contains(credential.id)}}; // 在线状态来自服务端绑定的连接。
        QString xianShiMing = credential.name; // 仅缩短发送副本，不改变磁盘中的原名称。
        constexpr int danTiaoShangXian = (protocol::maxWireBytes - 512) / 32; // 预留消息头、关联 ID 和数组分隔符空间。
        while (protocol::encode(chengYuan).toUtf8().size() > danTiaoShangXian && !xianShiMing.isEmpty()) { // 控制字符的 JSON 转义也计入实际字节数。
            xianShiMing.chop(1); // 每次从末尾缩短一个 UTF-16 单元直到成员记录可发送。
            if (!xianShiMing.isEmpty() && xianShiMing.back().isHighSurrogate()) xianShiMing.chop(1); // 不把代理对字符截成半个字符。
            chengYuan.insert("name", xianShiMing); // 更新发送副本后重新检查实际序列化大小。
        }
        devices.append(chengYuan); // 所有在线身份都能出现在同一份不超过 16 KiB 的名单中。
    }
    return devices;
}

void MessageServer::broadcastRoster()
{
    const auto message = protocol::message(QStringLiteral("device.list"), {{"devices", roster()}});
    const auto sockets = online_.values();
    for (auto* socket : sockets) send(socket, message);
}

// 在事件循环中逐条处理聊天室任务；只向当前在线的已认证设备广播。
void MessageServer::fenFaLiaoTian()
{
    liaoTianYipaicheng_ = false; // 当前排空任务已经开始，允许后续消息重新调度。
    while (!liaoTianDuilie_.isEmpty()) { // 保持服务器接收顺序。
        const auto task = liaoTianDuilie_.dequeue(); // 从有界队列移除一条任务。
        if (!peers_.contains(task.sender) || peers_.value(task.sender).deviceId.isEmpty()) continue; // 断开的发送者不再广播。
        const auto senderId = peers_.value(task.sender).deviceId; // 从认证连接取得可信发送者。
        auto event = protocol::message(QStringLiteral("chat.deliver"), {{"text", task.text},
            {"sequence", static_cast<qint64>(++liaoTianXuhao_)}}); // 服务器分配房间内顺序。
        event.insert("from", senderId); // 防止客户端伪造显示身份。
        event.insert("request_id", task.requestId); // 发送者据此匹配自己的消息。
        event.insert("at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)); // 统一服务器时间。
        for (auto* target : online_.values()) send(target, event); // 包括发送者，所有在线设备收到同一事件。
        reply(task.sender, QStringLiteral("chat.accepted"), task.requestId); // 仅表示服务器完成广播排队，不承诺每端已阅读。
        emit activity(QStringLiteral("聊天室消息：%1 #%2").arg(senderId).arg(liaoTianXuhao_)); // 日志只显示身份与序号，不记录正文。
    }
}

// 校验登录或自动登记请求；登记必须保存成功才上线，聊天仍按原队列顺序分发。
void MessageServer::receive(QWebSocket* socket, const QString& text)
{
    if (!peers_.contains(socket)) return;
    auto& peer = peers_[socket];
    const auto now = clock_.elapsed();
    if (now - peer.rateStart >= 10000) { peer.rateStart = now; peer.rateCount = 0; }
    if (++peer.rateCount > 120) {
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("rate_limit"));
        return;
    }
    QJsonObject message;
    if (!protocol::decode(text, &message)) {
        socket->close(QWebSocketProtocol::CloseCodeProtocolError, QStringLiteral("invalid_message"));
        return;
    }
    const auto requestId = message.value("id").toString();
    const auto type = message.value("type").toString();
    const auto payload = message.value("payload").toObject();
    if (peer.requests.contains(requestId)) { fail(socket, requestId, "duplicate_id"); return; }
    // Bound deduplication memory without silently forgetting IDs within a connection.
    if (peer.requests.size() >= 8192) {
        socket->close(QWebSocketProtocol::CloseCodeGoingAway, QStringLiteral("renew_connection"));
        return;
    }
    peer.requests.insert(requestId);
    if (peer.deviceId.isEmpty()) {
        if (type != "auth.login" && type != "auth.register") { fail(socket, requestId, "unauthorized"); return; } // 服务端保留协议兼容，但客户端只会发送自动登记请求。
        const auto deviceId = payload.value("device_id").toString(); // 每个连接绑定客户端独立 ID。
        const auto token = payload.value("token").toString(); // 令牌只交给注册模块校验。
        if (type == "auth.register" && zhuCeBiao_.kaiFang()) { // 开放模式允许新设备主动登记；固定名单也由下面的令牌校验兼容新版客户端。
            const bool yiDengJi = zhuCeBiao_.chaZhao(deviceId) != nullptr; // 相同身份重试不重复记录登记日志。
            const auto cuoWu = zhuCeBiao_.dengJi({deviceId, payload.value("name").toString(), token}); // 先原子保存再允许登录。
            if (!cuoWu.isEmpty()) { // 写入失败、格式错误和身份冲突均不能上线。
                fail(socket, requestId, cuoWu); // 返回可由客户端翻译的协议错误码。
                socket->close(QWebSocketProtocol::CloseCodePolicyViolated, cuoWu); // 结束未通过的登记连接。
                return; // 不进入聊天权限状态。
            }
            if (!yiDengJi) emit activity(QStringLiteral("设备注册成功：%1").arg(deviceId)); // 日志不包含令牌。
        }
        if (!zhuCeBiao_.yanZheng(deviceId, token)) { // 旧身份仍需令牌一致，开放加入不允许冒用身份。
            fail(socket, requestId, "auth_failed"); // 已有身份仍必须通过令牌校验，不能冒用注册身份。
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("auth_failed")); // 拒绝错误令牌。
            return; // 不能广播或发送消息。
        }
        if (online_.contains(deviceId)) { fail(socket, requestId, "already_online"); return; } // 防止同一身份同时绑定两个连接。
        peer.deviceId = deviceId; // 从此消息来源由服务端绑定，客户端不能自行伪造。
        online_.insert(deviceId, socket); // 登记已认证的在线连接。
        reply(socket, "auth.result", requestId, {{"device_id", deviceId}, {"name", zhuCeBiao_.chaZhao(deviceId)->name}}); // 回复登录成功，不回传令牌。
        emit activity(QStringLiteral("设备上线：%1").arg(deviceId)); // 原有中文上线日志继续保留。
        broadcastRoster(); // 让所有客户端立即看见新成员。
        return; // 本次身份请求已处理完成。
    }
    if (type == "device.list") {
        reply(socket, "device.list", requestId, {{"devices", roster()}});
    } else if (type == "chat.send") { // 公共房间与原私聊协议互不干扰。
        const auto body = payload.value("text").toString(); // 只接受纯文本字段。
        if (body.trimmed().isEmpty() || body.size() > protocol::maxTextLength) { fail(socket, requestId, "invalid_text"); return; } // 拒绝空白和超长内容。
        if (liaoTianDuilie_.size() >= 256) { fail(socket, requestId, "busy"); return; } // 队列满时显式失败，不无限占内存。
        liaoTianDuilie_.enqueue({socket, requestId, body}); // 认证成功后才允许入队。
        if (!liaoTianYipaicheng_) { // 每一轮事件循环只安排一次排空。
            liaoTianYipaicheng_ = true; // 标记已有待执行任务。
            QTimer::singleShot(0, this, &MessageServer::fenFaLiaoTian); // 异步分发，避免收包回调直接广播。
        }
    } else if (type == "message.send") {
        const auto targetId = message.value("to").toString();
        const auto body = payload.value("text").toString();
        if (body.trimmed().isEmpty() || body.size() > protocol::maxTextLength) {
            fail(socket, requestId, "invalid_text"); return;
        }
        if (targetId == peer.deviceId) { fail(socket, requestId, "self_target"); return; }
        auto* target = online_.value(targetId, nullptr);
        if (!target) { fail(socket, requestId, "target_offline"); return; }
        int outstanding = 0;
        for (const auto& pending : pending_) if (pending.sender == socket) ++outstanding;
        if (outstanding >= 16 || pending_.size() >= 256) { fail(socket, requestId, "busy"); return; }
        auto event = protocol::message("message.deliver", {{"text", body}});
        event.insert("from", peer.deviceId);
        event.insert("to", targetId);
        event.insert("at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        const auto deliveryId = event.value("id").toString();
        pending_.insert(deliveryId, {socket, target, requestId, now + 5000});
        if (!send(target, event)) {
            pending_.remove(deliveryId);
            fail(socket, requestId, "target_unavailable");
            return;
        }
        reply(socket, "message.accepted", requestId);
    } else if (type == "message.received") {
        const auto deliveryId = payload.value("message_id").toString();
        const auto it = pending_.find(deliveryId);
        if (it == pending_.end() || it->target != socket) {
            fail(socket, requestId, "invalid_receipt"); return;
        }
        const auto pending = *it;
        pending_.erase(it);
        reply(pending.sender, "message.received", pending.requestId);
    } else {
        fail(socket, requestId, "unknown_type");
    }
}

void MessageServer::disconnected(QWebSocket* socket)
{
    if (!peers_.contains(socket)) return;
    const auto deviceId = peers_.take(socket).deviceId;
    if (!deviceId.isEmpty()) {
        online_.remove(deviceId);
        emit activity(QStringLiteral("设备离线：%1").arg(deviceId)); // 使用中文日志记录设备断开连接。
    }
    const auto ids = pending_.keys();
    for (const auto& id : ids) {
        const auto pending = pending_.value(id);
        if (pending.sender == socket || pending.target == socket) {
            pending_.remove(id);
            if (pending.sender != socket) fail(pending.sender, pending.requestId, "delivery_unknown");
        }
    }
    socket->deleteLater();
    if (!deviceId.isEmpty()) broadcastRoster();
}

void MessageServer::tick()
{
    const auto now = clock_.elapsed();
    const auto ids = pending_.keys();
    for (const auto& id : ids) {
        const auto pending = pending_.value(id);
        if (now >= pending.expiresAt) {
            pending_.remove(id);
            fail(pending.sender, pending.requestId, "delivery_unknown");
        }
    }
    const auto sockets = peers_.keys();
    for (auto* socket : sockets) {
        if (!peers_.contains(socket)) continue;
        const auto peer = peers_.value(socket);
        if ((peer.deviceId.isEmpty() && now - peer.connectedAt > 5000)
            || now - peer.lastPong > 30000) socket->abort();
        else socket->ping();
    }
}
