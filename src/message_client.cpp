#include "message_client.h"
#include "protocol.h"
#include "keHuDuanPeiZhi.h" // 保存自动身份和最近成功的服务器入口。
#include <QHostAddress>

// 初始化网络事件；统一登记本机身份并在网络失败时切换备用入口。
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
        emit problem(QStringLiteral("连接或认证超时，正在重新尝试。")); // 超时不代表密码错误。
        socket_.abort(); // 关闭未完成连接，断线回调可能先安排重试。
        anPaiChongLian(); // 没有断线信号时也能继续尝试，且不会重复切换。
    });
    connect(&socket_, &QWebSocket::connected, this, [this] {
        emit stateChanged(QStringLiteral("正在登记设备"), false); // 客户端统一通过自动登记进入服务端。
        const auto request = protocol::message(QStringLiteral("auth.register"), // 所有客户端首次连接都通过自动登记进入服务端。
            {{"device_id", profile_.deviceId}, {"token", profile_.token}, {"name", profile_.sheBeiMing}}); // 每台电脑提交自己的独立身份。
        authRequest_ = request.value("id").toString(); // 只处理当前请求的认证结果。
        send(request); // 注册与登录共用现有 WebSocket JSON 通道。
    });
    connect(&socket_, &QWebSocket::textMessageReceived, this, &MessageClient::receive);
    connect(&socket_, &QWebSocket::errorOccurred, this, [this] {
        emit problem(QStringLiteral("无法连接服务器 %1（网络错误码：%2）。") // 不直接展示 Qt 的英文系统错误。
            .arg(profile_.server.toString()).arg(static_cast<int>(socket_.error()))); // 地址与错误码便于排查，令牌不输出。
        anPaiChongLian(); // 网络错误和断线可能连续发生，由统一方法去重。
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
        anPaiChongLian(); // 连接中断时尝试其余入口，仍保留原身份。
    });
    connect(&socket_, &QWebSocket::pong, this, [this] { lastPong_ = clock_.elapsed(); });
    connect(&maintenance_, &QTimer::timeout, this, [this] {
        const auto now = clock_.elapsed();
        if (now - lastPong_ > 20000) { socket_.abort(); return; }
        socket_.ping();
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

// 校验主入口、备用入口和设备身份；自动模式必须提供合法显示名和独立凭据。
QString MessageClient::validate(const ClientProfile& profile)
{
    QList<QUrl> diZhiLieBiao{profile.server}; // 主入口也需要和备用入口接受同一套检查。
    diZhiLieBiao.append(profile.beiYongDiZhi); // 避免重连时使用未经校验的备用 URL。
    if (diZhiLieBiao.size() > 8) return QStringLiteral("服务器入口最多可配置 8 个。"); // 限制配置和重试队列大小。
    for (const auto& url : diZhiLieBiao) { // 校验每个可能使用的入口。
        if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() // 地址必须有效且不允许内嵌账号。
            || url.hasQuery() || url.hasFragment() || (!url.path().isEmpty() && url.path() != "/") // 保持现有直接 WebSocket 入口约定。
            || (url.scheme() != "ws" && url.scheme() != "wss")) // 只接受支持的协议。
            return QStringLiteral("服务器地址必须是 ws:// 或 wss://，不包含账号、路径或查询参数"); // 主备用地址错误使用同一中文说明。
        if (url.scheme() == "ws" && !profile.allowInsecureLan // 未明确允许时不能使用非本机明文入口。
            && !QHostAddress(url.host()).isLoopback() // 回环地址仍可用于本机测试。
            && url.host().compare("localhost", Qt::CaseInsensitive) != 0) // localhost 也属于本机。
            return QStringLiteral("非本机连接必须使用 WSS"); // 不通过地址切换绕过原传输要求。
    }
    if (!protocol::validId(profile.deviceId)) return QStringLiteral("设备 ID 只能包含字母、数字、下划线和连字符，最长 80 位");
    if (profile.token.size() < 32 || profile.token.size() > 256)
        return QStringLiteral("访问令牌长度应为 32 到 256 位");
    if (profile.sheBeiMing.trimmed().isEmpty() || profile.sheBeiMing.size() > 80) // 自动登记必须有可展示的设备名。
        return QStringLiteral("自动登记的设备名称不能为空，且最多 80 个字符。"); // 在发请求前解释名称问题。
    return {};
}

// 开始用户要求的连接；重置本次自动身份恢复次数，不修改调用方的原参数。
void MessageClient::start(const ClientProfile& profile)
{
    stop();
    const auto error = validate(profile);
    if (!error.isEmpty()) { emit problem(error); return; }
    profile_ = profile;
    yiGengHuanShenFen_ = false; // 一次连接任务最多重新生成一次冲突身份。
    wanted_ = true;
    retryDelay_ = 1000;
    open();
}

// 打开当前入口并启动超时计时；网络失败由统一重连方法选择下一入口。
void MessageClient::open()
{
    if (!wanted_) return;
    const auto zhuangTai = profile_.server.host().startsWith(QStringLiteral("192.168.")) // 当前部署的局域网入口显示内网提示。
        ? QStringLiteral("正在连接内网") : QStringLiteral("正在连接服务器"); // 其他入口不假定一定是公网。
    emit stateChanged(zhuangTai, false); // 用户能看见按钮已执行连接动作。
    deadline_.start(8000);
    socket_.open(profile_.server);
}

// 每次网络失败只安排一次重试；循环尝试入口并逐步延长重试间隔。
void MessageClient::anPaiChongLian()
{
    if (!wanted_ || retry_.isActive()) return; // 用户停止或已有重试时不重复切换。
    deadline_.stop(); // 上一次连接的超时事件不再影响新尝试。
    if (!profile_.beiYongDiZhi.isEmpty()) { // 自动连接在主入口失败后轮换备用入口。
        const auto yuanDiZhi = profile_.server; // 旧入口留作以后的重试候选。
        profile_.server = profile_.beiYongDiZhi.takeFirst(); // 切到下一个候选入口。
        profile_.beiYongDiZhi.append(yuanDiZhi); // 内外网入口轮流尝试，不永久放弃任何一端。
        emit stateChanged(QStringLiteral("连接失败，等待切换入口"), false); // 解释下一次连接为什么使用不同地址。
    } else { // 没有备用地址时仍然支持原有自动重连。
        emit stateChanged(QStringLiteral("连接中断，等待重连"), false); // 保留明确的中文状态。
    }
    retry_.start(retryDelay_); // QTimer 通过事件循环稍后重试，不阻塞界面。
    retryDelay_ = qMin(retryDelay_ * 2, 15000); // 失败越多间隔越长，最大十五秒。
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

// 连接中断时将公共聊天室中尚未确认的发送统一标为结果未知。
void MessageClient::losePending()
{
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

// 处理登记或登录结果、中文错误和聊天广播；身份先保存，界面再确认上线。
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
        QString baoCunCuoWu; // 保存最近成功地址，供下次启动优先使用。
        if (!KeHuDuanPeiZhi::baoCun(profile_, &baoCunCuoWu)) { // 不能持久保存时不把该连接当作正常完成。
            wanted_ = false; // 阻止持续连接却丢失本机身份的状态。
            retry_.stop(); // 不使用失败的存储配置反复重试。
            deadline_.stop(); // 当前认证过程结束。
            socket_.abort(); // 服务端也会将这台设备标记离线。
            emit problem(baoCunCuoWu); // 用户看到具体文件问题。
            return; // 不再设置上线状态。
        }
        emit peiZhiGengXin(profile_); // 界面显示实际成功的入口和当前设备身份。
        online_ = true;
        deadline_.stop();
        retry_.stop(); // 认证成功后取消尚未执行的重试，避免正常连接被再次打开。
        retryDelay_ = 1000;
        lastPong_ = clock_.elapsed();
        maintenance_.start();
        emit stateChanged(QStringLiteral("已连接"), true);
    } else if (type == "error") {
        const auto code = payload.value("code").toString();
        if (replyId == authRequest_) {
            QString tiShi = QStringLiteral("设备 ID 或访问令牌不正确"); // 默认认证提示也可被更准确的文件错误替换。
            if (code == "auth_failed" && !yiGengHuanShenFen_) { // 身份冲突时允许自动生成一次新身份后重新登记。
                QString baoCunCuoWu; // 重新生成前先确保能够保存。
                if (KeHuDuanPeiZhi::chuangJianShenFen(&profile_, &baoCunCuoWu)) { // 从不要求服务端覆盖旧设备令牌。
                    yiGengHuanShenFen_ = true; // 限制本次任务只能恢复一次，避免无限注册。
                    emit peiZhiGengXin(profile_); // 界面同步新身份，后续发送者判断不再使用旧 ID。
                    emit problem(QStringLiteral("旧身份认证失败，已保存新身份，正在重新登记。")); // 明确显示恢复流程。
                    socket_.abort(); // 关闭被拒绝的旧连接。
                    anPaiChongLian(); // 使用保存后的同一新身份重试。
                    return; // 不进入下方终止逻辑。
                }
                tiShi = baoCunCuoWu; // 保留身份写入失败原因，不能随后又覆盖成令牌错误。
            }
            wanted_ = false; // 业务拒绝不伪装成网络失败循环切换。
            retry_.stop(); // 停止既有重试。
            deadline_.stop(); // 认证已收到明确结果，不再等待超时。
            socket_.abort(); // 回到未连接状态。
            if (code == "already_online") tiShi = QStringLiteral("该设备已在其他客户端上线"); // 同一身份不能同时用于两台客户端。
            else if (code == "registration_disabled" || code == "unauthorized") tiShi = QStringLiteral("服务端未开放自动注册，请更新服务端并启用自动注册。"); // 旧服务端需要升级。
            else if (code == "registration_write_failed") tiShi = QStringLiteral("服务端无法保存设备注册记录，请检查服务端目录权限。"); // 文件错误与密码错误分开。
            else if (code == "registration_full") tiShi = QStringLiteral("服务端已达到 4096 个注册身份上限。"); // 与同时在线数量区分。
            else if (code == "invalid_credentials") tiShi = QStringLiteral("设备登记信息无效，请检查设备名称和身份配置。"); // 解释格式校验失败。
            emit problem(tiShi); // 所有已知登记错误均使用中文说明。
        } else if (liaoTianDaifa_.remove(replyId)) { // 公共聊天室发送错误结算。
            const auto status = code == "delivery_unknown" ? QStringLiteral("结果未知")
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
    }
}
