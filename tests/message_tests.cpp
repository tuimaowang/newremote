#include "main_window.h"
#include "message_server.h"
#include "protocol.h"
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTextBrowser>
#include <QTextEdit>
#include <QWebSocket>

struct Probe {
    QList<QJsonObject> inbox;
    QWebSocket socket;
    Probe() {
        QObject::connect(&socket, &QWebSocket::textMessageReceived, &socket, [this](const QString& text) {
            inbox.append(QJsonDocument::fromJson(text.toUtf8()).object());
        });
    }
    void send(const QJsonObject& message) { socket.sendTextMessage(protocol::encode(message)); }
    bool has(const QString& type, const QString& reply = {}) const {
        for (const auto& message : inbox)
            if (message.value("type").toString() == type
                && (reply.isEmpty() || message.value("reply_to").toString() == reply)) return true;
        return false;
    }
    QJsonObject last(const QString& type) const {
        for (auto it = inbox.crbegin(); it != inbox.crend(); ++it)
            if (it->value("type").toString() == type) return *it;
        return {};
    }
};

class MessageTests : public QObject {
    Q_OBJECT
private:
    const QString tokenA = QString(40, 'a');
    const QString tokenB = QString(40, 'b');
    const QString tokenC = QString(40, 'c');
    std::unique_ptr<MessageServer> server;
    QUrl url;
    void login(Probe& peer, const QString& id, const QString& token) {
        peer.socket.open(url);
        QTRY_COMPARE(peer.socket.state(), QAbstractSocket::ConnectedState);
        peer.send(protocol::message("auth.login", {{"device_id", id}, {"token", token}}));
        QTRY_VERIFY(peer.has("auth.result"));
    }
private slots:
    void init() {
        server = std::make_unique<MessageServer>(QList<DeviceCredential>{
            {"device-a", QStringLiteral("开发电脑 A"), tokenA},
            {"device-b", QStringLiteral("测试电脑 B"), tokenB},
            {"device-c", QStringLiteral("备用电脑 C"), tokenC}});
        QVERIFY(server->listen(QHostAddress::LocalHost, 0));
        url = QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server->port()));
    }
    void cleanup() { server.reset(); }

    void protocolValidation() {
        QJsonObject decoded;
        QVERIFY(protocol::decode(protocol::encode(protocol::message("device.list")), &decoded));
        QVERIFY(!protocol::decode("[]", &decoded));
        QVERIFY(!protocol::decode("{", &decoded));
        auto message = protocol::message("device.list");
        message.insert("version", 1.5);
        QVERIFY(!protocol::decode(protocol::encode(message), &decoded));
        message.insert("version", 1);
        message.insert("id", QString(100, 'x'));
        QVERIFY(!protocol::decode(protocol::encode(message), &decoded));
        QVERIFY(!MessageClient::validate({QUrl("ws://192.168.1.2:17890"), "device-a", tokenA}).isEmpty());
        QVERIFY(MessageClient::validate({url, "device-a", tokenA}).isEmpty());
        QVERIFY(MessageClient::validate({QUrl("wss://example.com"), "device-a", tokenA}).isEmpty());
    }

    void plaintextCannotListenOnLan() {
        MessageServer external({{"device-a", "A", tokenA}});
        QVERIFY(!external.listen(QHostAddress::AnyIPv4, 0));
    }

    void unauthorizedAndBadToken() {
        Probe peer;
        peer.socket.open(url);
        QTRY_COMPARE(peer.socket.state(), QAbstractSocket::ConnectedState);
        peer.send(protocol::message("device.list"));
        QTRY_VERIFY(peer.has("error"));
        QCOMPARE(peer.last("error").value("payload").toObject().value("code").toString(), "unauthorized");
        QVERIFY(!peer.has("device.list"));
        peer.send(protocol::message("auth.login", {{"device_id", "device-a"}, {"token", tokenB}}));
        QTRY_COMPARE(peer.socket.state(), QAbstractSocket::UnconnectedState);
        QVERIFY(!peer.has("auth.result"));
    }

    void duplicateIdentityRejected() {
        Probe first;
        Probe second;
        login(first, "device-a", tokenA);
        second.socket.open(url);
        QTRY_COMPARE(second.socket.state(), QAbstractSocket::ConnectedState);
        second.send(protocol::message("auth.login", {{"device_id", "device-a"}, {"token", tokenA}}));
        QTRY_VERIFY(second.has("error"));
        QCOMPARE(second.last("error").value("payload").toObject().value("code").toString(), "already_online");
        QCOMPARE(first.socket.state(), QAbstractSocket::ConnectedState);
    }

    void relayBindsSenderAndReceipt() {
        Probe a;
        Probe b;
        Probe c;
        login(a, "device-a", tokenA);
        login(b, "device-b", tokenB);
        login(c, "device-c", tokenC);
        const auto body = QStringLiteral("中文 <script> & \n第二行");
        auto request = protocol::message("message.send", {{"text", body}});
        request.insert("from", "device-c");
        request.insert("to", "device-b");
        const auto id = request.value("id").toString();
        a.send(request);
        QTRY_VERIFY(b.has("message.deliver"));
        QTRY_VERIFY(a.has("message.accepted", id));
        const auto delivered = b.last("message.deliver");
        QCOMPARE(delivered.value("from").toString(), "device-a");
        QCOMPARE(delivered.value("payload").toObject().value("text").toString(), body);
        QVERIFY(!a.has("message.received", id));
        c.send(protocol::message("message.received", {{"message_id", delivered.value("id")}}));
        QTRY_VERIFY(c.has("error"));
        QCOMPARE(c.last("error").value("payload").toObject().value("code").toString(), "invalid_receipt");
        QVERIFY(!a.has("message.received", id));
        b.send(protocol::message("message.received", {{"message_id", delivered.value("id")}}));
        QTRY_VERIFY(a.has("message.received", id));
        a.send(request);
        QTRY_VERIFY(a.has("error", id));
        QCOMPARE(a.last("error").value("payload").toObject().value("code").toString(), "duplicate_id");
        int deliveries = 0;
        for (const auto& value : b.inbox) if (value.value("type") == "message.deliver") ++deliveries;
        QCOMPARE(deliveries, 1);
    }

    void offlineAndOversizedText() {
        Probe a;
        login(a, "device-a", tokenA);
        auto request = protocol::message("message.send", {{"text", "hello"}});
        request.insert("to", "device-b");
        a.send(request);
        QTRY_VERIFY(a.has("error", request.value("id").toString()));
        QCOMPARE(a.last("error").value("payload").toObject().value("code").toString(), "target_offline");
        request = protocol::message("message.send", {{"text", QString(4001, 'x')}});
        request.insert("to", "device-b");
        a.send(request);
        QTRY_VERIFY(a.has("error", request.value("id").toString()));
        QCOMPARE(a.last("error").value("payload").toObject().value("code").toString(), "invalid_text");
    }

    void malformedAndOversizedWireClose() {
        QTest::failOnWarning(QRegularExpression(".*Failed to create a timer.*"));
        Probe a;
        a.socket.open(url);
        QTRY_COMPARE(a.socket.state(), QAbstractSocket::ConnectedState);
        a.socket.sendTextMessage("not json");
        QTRY_COMPARE(a.socket.state(), QAbstractSocket::UnconnectedState);
        Probe b;
        b.socket.open(url);
        QTRY_COMPARE(b.socket.state(), QAbstractSocket::ConnectedState);
        b.socket.sendTextMessage(QString(20000, 'x'));
        QTRY_COMPARE(b.socket.state(), QAbstractSocket::UnconnectedState);
    }

    void clientRejectsOversizedServerFrame() {
        QTest::failOnWarning(QRegularExpression(".*Failed to create a timer.*"));
        QWebSocketServer badServer("oversized", QWebSocketServer::NonSecureMode);
        QVERIFY(badServer.listen(QHostAddress::LocalHost, 0));
        MessageClient client;
        QSignalSpy problems(&client, &MessageClient::problem);
        client.start({QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(badServer.serverPort())),
                      "device-a", tokenA});
        QTRY_VERIFY(badServer.hasPendingConnections());
        std::unique_ptr<QWebSocket> peer(badServer.nextPendingConnection());
        peer->sendTextMessage(QString(20000, 'x'));
        QTRY_VERIFY(!client.active());
        QTRY_COMPARE(peer->state(), QAbstractSocket::UnconnectedState);
        QVERIFY(!problems.isEmpty());
    }

    void deliveryTimeoutIsUnknown() {
        Probe a;
        Probe b;
        login(a, "device-a", tokenA);
        login(b, "device-b", tokenB);
        auto request = protocol::message("message.send", {{"text", "no receipt"}});
        request.insert("to", "device-b");
        a.send(request);
        QTRY_VERIFY(b.has("message.deliver"));
        QTRY_VERIFY_WITH_TIMEOUT(a.has("error", request.value("id").toString()), 7500);
        QCOMPARE(a.last("error").value("payload").toObject().value("code").toString(), "delivery_unknown");
    }

    void targetDisconnectSettlesPending() {
        Probe a;
        Probe b;
        login(a, "device-a", tokenA);
        login(b, "device-b", tokenB);
        auto request = protocol::message("message.send", {{"text", "disconnect"}});
        request.insert("to", "device-b");
        a.send(request);
        QTRY_VERIFY(b.has("message.deliver"));
        b.socket.abort();
        QTRY_VERIFY(a.has("error", request.value("id").toString()));
        QCOMPARE(a.last("error").value("payload").toObject().value("code").toString(), "delivery_unknown");
        QTRY_VERIFY(!a.last("device.list").value("payload").toObject().value("devices").toArray()
                        .at(1).toObject().value("online").toBool());
    }

    void clientRoundTripAndReconnect() {
        MessageClient a;
        MessageClient b;
        QSignalSpy incoming(&b, &MessageClient::incoming);
        QSignalSpy status(&a, &MessageClient::deliveryChanged);
        a.start({url, "device-a", tokenA});
        b.start({url, "device-b", tokenB});
        QTRY_VERIFY(a.online() && b.online());
        const auto id = a.sendMessage("device-b", QStringLiteral("收到请回复"));
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE(incoming.count(), 1);
        QTRY_COMPARE(status.count(), 2);
        QCOMPARE(status.last().at(1).toString(), QStringLiteral("目标已收到"));
        const auto port = server->port();
        server->stop();
        QTRY_VERIFY(!a.online() && !b.online());
        QVERIFY(server->listen(QHostAddress::LocalHost, port));
        QTRY_VERIFY_WITH_TIMEOUT(a.online() && b.online(), 6000);
        QCOMPARE(incoming.count(), 1);
        a.stop();
        b.stop();
        QVERIFY(!a.active() && !b.active());
    }

    // 验证统一房间对三台在线设备广播、按序编号，且拒绝非法正文与重复请求。
    void gongGongLiaoTianGuangBo() // 使用真实 WebSocket 检查服务器房间协议。
    {
        Probe a; Probe b; Probe c; // 三个独立的认证连接。
        login(a, "device-a", tokenA); // 登录第一台设备。
        login(b, "device-b", tokenB); // 登录第二台设备。
        login(c, "device-c", tokenC); // 登录第三台设备。
        auto first = protocol::message("chat.send", {{"text", QStringLiteral("公共消息 <b>测试</b>")}}); // 构造纯文本内容。
        first.insert("from", "device-c"); // 尝试伪造发送者。
        a.send(first); // 由第一台设备实际发送。
        QTRY_VERIFY(a.has("chat.deliver") && b.has("chat.deliver") && c.has("chat.deliver")); // 包括发送者均能收到。
        QTRY_VERIFY(a.has("chat.accepted", first.value("id").toString())); // 服务器确认已广播。
        QCOMPARE(b.last("chat.deliver").value("from").toString(), "device-a"); // 来源以认证连接为准。
        QCOMPARE(b.last("chat.deliver").value("payload").toObject().value("sequence").toInt(), 1); // 首条编号。
        QCOMPARE(c.last("chat.deliver").value("payload").toObject().value("text").toString(), QStringLiteral("公共消息 <b>测试</b>")); // 正文不被服务器当 HTML。
        b.send(protocol::message("chat.send", {{"text", "second"}})); // 第二台设备发言。
        QTRY_VERIFY(a.last("chat.deliver").value("payload").toObject().value("sequence").toInt() == 2); // 所有人共享序号。
        a.send(first); // 重发原请求 ID。
        QTRY_VERIFY(a.has("error", first.value("id").toString())); // 重复 ID 被拒绝。
        a.send(protocol::message("chat.send", {{"text", "  "}})); // 不允许空白内容。
        QTRY_COMPARE(a.last("error").value("payload").toObject().value("code").toString(), "invalid_text"); // 返回明确错误。
    }

    // 验证客户端 API 收到同一广播并能明确确认发送状态。
    void gongGongLiaoTianKehuDuan() // 两个客户端共用房间，无需选择目标。
    {
        MessageClient a; MessageClient b; // 创建两个真实客户端。
        QSignalSpy arrived(&b, &MessageClient::liaoTianDaoDa); // 观察房间广播。
        QSignalSpy state(&a, &MessageClient::deliveryChanged); // 观察服务器确认。
        a.start({url, "device-a", tokenA}); // 第一台上线。
        b.start({url, "device-b", tokenB}); // 第二台上线。
        QTRY_VERIFY(a.online() && b.online()); // 等待认证完成。
        const auto id = a.faSongLiaoTian(QStringLiteral("内外网测试")); // 不指定目标发送。
        QVERIFY(!id.isEmpty()); // 请求已写入连接。
        QTRY_COMPARE(arrived.count(), 1); // 第二台收到房间消息。
        QCOMPARE(arrived.first().at(1).toString(), "device-a"); // 展示可信来源。
        QTRY_VERIFY(!state.isEmpty()); // 第一台收到确认。
        QCOMPARE(state.last().at(1).toString(), QStringLiteral("服务器已广播")); // 不声称已阅读。
    }

    // 验证公共房间 UI 的发送、成员状态和紧凑窗口截图。
    void widgetMessageFlowAndScreenshots() {
        MainWindow a({url, "device-a", tokenA});
        MainWindow b({url, "device-b", tokenB});
        a.show();
        b.show();
        a.connectToServer();
        b.connectToServer();
        QTRY_VERIFY(a.client()->online() && b.client()->online());
        auto* list = a.findChild<QListWidget*>("deviceList");
        QTRY_COMPARE(list->count(), 2);
        QCOMPARE(list->count(), 2); // 设备列表只用于查看成员。
        auto* input = a.findChild<QTextEdit*>("input");
        auto* send = a.findChild<QPushButton*>("primary");
        auto* historyA = a.findChild<QTextBrowser*>("messageHistory");
        auto* historyB = b.findChild<QTextBrowser*>("messageHistory");
        QVERIFY(!send->isEnabled()); // 输入为空时不能发送。
        input->setPlainText(QStringLiteral("你好，消息通道已连接。\n中文、换行和 <b>纯文本</b> 均可正常传输。"));
        QVERIFY(send->isEnabled());
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_VERIFY(historyB->toPlainText().contains(QStringLiteral("<b>纯文本</b>"))); // 接收端展示原始文本。
        QTRY_VERIFY(historyA->toPlainText().contains(QStringLiteral("服务器已广播"))); // 本机显示广播结果。
        QVERIFY(input->toPlainText().isEmpty());
        auto* inputB = b.findChild<QTextEdit*>("input");
        inputB->setPlainText(QStringLiteral("已收到。双向消息和回执正常。"));
        QTest::mouseClick(b.findChild<QPushButton*>("primary"), Qt::LeftButton);
        QTRY_VERIFY(historyA->toPlainText().contains(QStringLiteral("双向消息和回执正常")));
        QDir().mkpath("screenshots");
        QTest::qWait(100);
        QVERIFY(a.grab().save("screenshots/messages-desktop.png"));
        a.resize(760, 540);
        QTest::qWait(100);
        QVERIFY(a.grab().save("screenshots/messages-compact.png"));
        QVERIFY(send->geometry().right() <= send->parentWidget()->width());
        input->setPlainText(QString(4001, 'x'));
        QVERIFY(!send->isEnabled());
        b.client()->stop();
        QTRY_VERIFY(a.findChild<QLabel*>("heading") != nullptr);
        QTest::qWait(150);
        input->setPlainText("offline");
        QTRY_VERIFY(send->isEnabled()); // 其他设备离线时公共房间仍允许本机发送。
        a.client()->stop(); // 本机断线后才禁用发送。
        QTRY_VERIFY(!send->isEnabled()); // 不能在离线状态提交消息。
    }
};

QTEST_MAIN(MessageTests)
#include "message_tests.moc"
