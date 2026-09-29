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

bool MessageServer::listen(const QHostAddress& address, quint16 port,
                           const QSslConfiguration* tls, bool allowInsecureLan)
{
    stop();
    error_.clear();
    if (!tls && !address.isLoopback() && !allowInsecureLan) {
        error_ = QStringLiteral("Plain WebSocket is restricted to loopback; configure TLS for LAN access.");
        return false;
    }
    QSet<QString> ids;
    QSet<QString> tokens;
    for (const auto& credential : credentials_) {
        if (!protocol::validId(credential.id) || credential.name.trimmed().isEmpty()
            || credential.name.size() > 80 || credential.token.size() < 32
            || credential.token.size() > 256 || ids.contains(credential.id)
            || tokens.contains(credential.token)) {
            error_ = QStringLiteral("Invalid or duplicate device credentials.");
            return false;
        }
        ids.insert(credential.id);
        tokens.insert(credential.token);
    }
    if (credentials_.isEmpty() || credentials_.size() > 32) {
        error_ = QStringLiteral("Configure between 1 and 32 devices.");
        return false;
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

QJsonArray MessageServer::roster() const
{
    QJsonArray devices;
    for (const auto& credential : credentials_) {
        devices.append(QJsonObject{{"id", credential.id}, {"name", credential.name},
                                  {"online", online_.contains(credential.id)}});
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
        emit activity(QStringLiteral("chat %1 #%2").arg(senderId).arg(liaoTianXuhao_)); // 日志只显示身份与序号，不记录正文。
    }
}

// 校验认证连接的请求；聊天室文本先入有界队列，再由事件循环分发。
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
        if (type != "auth.login") { fail(socket, requestId, "unauthorized"); return; }
        const auto deviceId = payload.value("device_id").toString();
        const auto token = payload.value("token").toString().toUtf8();
        for (const auto& credential : credentials_) {
            if (credential.id != deviceId) continue;
            const auto expected = credential.token.toUtf8();
            unsigned int different = static_cast<unsigned int>(expected.size() ^ token.size());
            for (qsizetype i = 0; i < expected.size(); ++i)
                different |= static_cast<unsigned char>(expected[i])
                    ^ static_cast<unsigned char>(i < token.size() ? token[i] : 0);
            if (different != 0) break;
            if (online_.contains(deviceId)) { fail(socket, requestId, "already_online"); return; }
            peer.deviceId = deviceId;
            online_.insert(deviceId, socket);
            reply(socket, "auth.result", requestId, {{"device_id", deviceId}, {"name", credential.name}});
            emit activity(QStringLiteral("online %1").arg(deviceId));
            broadcastRoster();
            return;
        }
        fail(socket, requestId, "auth_failed");
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("auth_failed"));
        return;
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
        emit activity(QStringLiteral("offline %1").arg(deviceId));
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
