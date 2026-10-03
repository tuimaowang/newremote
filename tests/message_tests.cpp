#include "main_window.h"
#include "message_server.h"
#include "protocol.h"
#include "keHuDuanPeiZhi.h" // 验证首次运行保存身份及再次读取的行为。
#include "sheBeiZhuCe.h" // 验证注册记录的持久化和冲突保护。
#include <QFile> // 构造损坏配置或写入失败的测试条件。
#include <QTemporaryDir> // 身份和注册记录写入临时目录，不触碰真实部署配置。
#include <QTcpServer> // 获取一个已关闭的本机端口来验证备用地址切换。
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTextBrowser>
#include <QTextEdit>
#include <QWebSocket>
#include <vector> // 保存不能复制的测试连接，并在测试结束后自动释放。

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

    // 验证新电脑自动生成独立身份、身份恢复和入口保存；客户端不再支持固定配置模式。
    void ziDongPeiZhiBaoCun() // 所有文件限定在 Qt 自动清理的临时目录。
    {
        QTemporaryDir muLu; // 模拟一台没有任何配置的新电脑。
        QVERIFY(muLu.isValid()); // 后续文件不会误写真实 AppData。
        const auto luJing = muLu.filePath("client-identity.json"); // 模拟本机专属身份文件。
        ClientProfile a; ClientProfile b; // 模拟两台不同客户端。
        QString cuoWu; // 保存配置模块的中文错误。
        QVERIFY(KeHuDuanPeiZhi::duQuZiDong(luJing, &a, &cuoWu)); // 不预先提供 JSON 也能创建身份。
        QVERIFY(!a.deviceId.isEmpty() && !a.token.isEmpty() && QFile::exists(luJing)); // 自动注册前身份已经永久保存。
        QVERIFY(KeHuDuanPeiZhi::duQuZiDong(muLu.filePath("second.json"), &b, &cuoWu)); // 另一台客户端独立生成身份。
        QVERIFY(a.deviceId != b.deviceId && a.token != b.token); // 不允许所有新电脑使用相同身份。
        std::swap(a.server, a.beiYongDiZhi.first()); // 模拟公网成功，下一次启动应优先公网。
        QVERIFY(KeHuDuanPeiZhi::baoCun(a, &cuoWu)); // 将最近成功的入口保存。
        ClientProfile huiFu; // 模拟关闭客户端后的重新启动。
        QVERIFY(KeHuDuanPeiZhi::duQuZiDong(luJing, &huiFu, &cuoWu)); // 重新读取先前身份。
        QCOMPARE(huiFu.deviceId, a.deviceId); // ID 不因重启重新生成。
        QCOMPARE(huiFu.token, a.token); // 登录凭据保持一致。
        QCOMPARE(huiFu.server, a.server); // 成功地址成为默认入口。
        QCOMPARE(huiFu.beiYongDiZhi, a.beiYongDiZhi); // 另一个入口仍可在失败时使用。
        QFile sunHuai(muLu.filePath("broken.json")); // 构造损坏配置，验证不能悄悄重置身份。
        QVERIFY(sunHuai.open(QIODevice::WriteOnly)); // 临时文件可写。
        QCOMPARE(sunHuai.write("{"), qint64(1)); // 写入无效 JSON。
        sunHuai.close(); // 读取前关闭写句柄，Windows 不产生共享冲突。
        QVERIFY(!KeHuDuanPeiZhi::duQuZiDong(sunHuai.fileName(), &huiFu, &cuoWu)); // 必须报告损坏文件。
        QVERIFY(cuoWu.contains(QStringLiteral("JSON"))); // 错误指向配置语法而不是服务器密码。
    }

    // 验证注册可重复、错误令牌不能覆盖身份、累计设备不受并发 32 台限制且重启可恢复。
    void zhuCeJiLuChiJiuHua() // 直接测试身份库，并隔离真实 server.json。
    {
        QTemporaryDir muLu; // 临时数据库由测试结束后清理。
        QVERIFY(muLu.isValid()); // 文件位置必须可用。
        const auto luJing = muLu.filePath("registered-devices.json"); // 模拟永久注册记录。
        SheBeiZhuCe zhuCe; // 模拟首次启动且没有预设设备。
        QString cuoWu; // 返回中文文件错误。
        QVERIFY(zhuCe.duQu({}, luJing, &cuoWu)); // 开放模式支持空初始设备表。
        const DeviceCredential pingJu{"new-device", QStringLiteral("测试设备"), tokenA}; // 固定测试凭据不打印。
        QVERIFY(zhuCe.dengJi(pingJu).isEmpty()); // 初次登记永久保存。
        QVERIFY(zhuCe.dengJi(pingJu).isEmpty()); // 注册响应丢失后重试仍然成功。
        QCOMPARE(zhuCe.sheBei().size(), 1); // 重试不会重复创建身份。
        QCOMPARE(zhuCe.dengJi({"new-device", QStringLiteral("冒用"), tokenB}), QStringLiteral("auth_failed")); // 相同 ID 不同令牌不能覆盖。
        QVERIFY(zhuCe.yanZheng(pingJu.id, tokenA)); // 原凭据仍然有效。
        for (int i = 0; i < 34; ++i) { // 累计超过三十二台，证明与在线容量分离。
            const DeviceCredential xin{"extra-" + QString::number(i), QStringLiteral("额外设备"), // 每条记录具有独立 ID。
                QString(40, 'x') + QString::number(i)}; // 每台身份也使用独立测试令牌。
            QVERIFY(zhuCe.dengJi(xin).isEmpty()); // 第三十三个永久身份也可以登记。
        }
        SheBeiZhuCe huiFu; // 模拟服务端进程完全重建身份模块。
        QVERIFY(huiFu.duQu({}, luJing, &cuoWu)); // 从磁盘而不是上一个对象恢复。
        QCOMPARE(huiFu.sheBei().size(), 35); // 全部身份仍然存在。
        QVERIFY(huiFu.yanZheng(pingJu.id, tokenA)); // 重启后的令牌仍可认证。
        QFile sunHuai(luJing); // 模拟身份库意外损坏。
        QVERIFY(sunHuai.open(QIODevice::WriteOnly | QIODevice::Truncate)); // 只破坏临时测试数据库。
        QCOMPARE(sunHuai.write("bad"), qint64(3)); // 生成无效 JSON。
        sunHuai.close(); // 释放写句柄。
        QVERIFY(!huiFu.duQu({}, luJing, &cuoWu)); // 加载失败不能清空身份库。
        QVERIFY(huiFu.yanZheng(pingJu.id, tokenA)); // 失败后运行中的原记录保持不变。
    }

    // 验证磁盘写入失败不会在内存里登记成功，并向客户端返回具体中文原因。
    void zhuCeXieRuShiBai() // 将普通文件当作父目录来制造可靠的跨平台写入失败。
    {
        QTemporaryDir muLu; // 不依赖真实磁盘权限变化。
        QVERIFY(muLu.isValid()); // 临时环境可用。
        QFile zuDang(muLu.filePath("not-a-directory")); // 普通文件不能作为身份库的父目录。
        QVERIFY(zuDang.open(QIODevice::WriteOnly)); // 创建路径阻挡条件。
        zuDang.close(); // 保持文件存在，但释放句柄。
        const auto luJing = zuDang.fileName() + "/devices.json"; // 该路径不可能成功创建父目录。
        MessageServer kaiFang({}); // 模拟空固定名单的开放服务端。
        QVERIFY(kaiFang.qiYongZhuCe(luJing)); // 身份库不存在时可以启动，实际登记时检验写权限。
        QVERIFY(kaiFang.listen(QHostAddress::LocalHost, 0)); // 测试服务仅监听本机临时端口。
        ClientProfile peiZhi{QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(kaiFang.port())), "write-fails", tokenA}; // 凭据格式本身合法。
        peiZhi.sheBeiMing = QStringLiteral("写入测试"); // 自动登记必须提供可展示的设备名称。
        MessageClient keHuDuan; // 观察真实网络层收到的错误。
        QSignalSpy cuoWu(&keHuDuan, &MessageClient::problem); // 捕获界面应展示的中文文案。
        keHuDuan.start(peiZhi); // 发起登记，服务端不能保存。
        QTRY_VERIFY(!keHuDuan.active()); // 文件失败不会无限重试注册。
        QVERIFY(!keHuDuan.online()); // 保存失败不能声称上线。
        QVERIFY(!cuoWu.isEmpty()); // 界面得到可见反馈。
        QVERIFY(cuoWu.last().first().toString().contains(QStringLiteral("无法保存"))); // 与令牌不正确区分。
        QVERIFY(!QFile::exists(luJing)); // 没有产生半份身份库。
    }

    // 验证两个新客户端自动登记并聊天，以及服务端对象重建后凭据仍然有效。
    void ziDongZhuCeLiaoTianYuChongQi() // 使用真实 WebSocket，文件仅保存到临时目录。
    {
        QTemporaryDir muLu; // 隔离客户端和服务端记录。
        QVERIFY(muLu.isValid()); // 保证测试环境完整。
        const auto jiLu = muLu.filePath("devices.json"); // 服务端永久记录。
        auto kaiFang = std::make_unique<MessageServer>(QList<DeviceCredential>{}); // 不预设任何设备身份。
        QVERIFY(kaiFang->qiYongZhuCe(jiLu)); // 加载开放注册模式。
        QVERIFY(kaiFang->listen(QHostAddress::LocalHost, 0)); // 使用操作系统分配的本机端口。
        const QUrl ruKou(QStringLiteral("ws://127.0.0.1:%1").arg(kaiFang->port())); // 两个客户端访问同一服务端。
        ClientProfile a; ClientProfile b; QString cuoWu; // 准备两个独立电脑身份。
        QVERIFY(KeHuDuanPeiZhi::duQuZiDong(muLu.filePath("a.json"), &a, &cuoWu)); // 第一个电脑无配置启动。
        QVERIFY(KeHuDuanPeiZhi::duQuZiDong(muLu.filePath("b.json"), &b, &cuoWu)); // 第二个电脑也自动生成身份。
        a.server = ruKou; a.beiYongDiZhi.clear(); // 测试不访问真实内外网地址。
        b.server = ruKou; b.beiYongDiZhi.clear(); // 两个测试客户端都只访问本机。
        MessageClient keHuA; MessageClient keHuB; // 分别建立独立会话。
        QSignalSpy daoDa(&keHuB, &MessageClient::liaoTianDaoDa); // 检查自动身份也能接收原聊天协议。
        keHuA.start(a); keHuB.start(b); // 首次连接自动登记。
        QTRY_VERIFY(keHuA.online() && keHuB.online()); // 两个身份同时上线。
        QVERIFY(!keHuA.faSongLiaoTian(QStringLiteral("自动注册后的消息")).isEmpty()); // 原发送功能可继续使用。
        QTRY_COMPARE(daoDa.count(), 1); // 对方收到真实广播。
        QCOMPARE(daoDa.first().at(1).toString(), a.deviceId); // 消息来源由独立身份绑定。
        const auto duanKou = kaiFang->port(); // 重启后保持同一入口。
        kaiFang.reset(); // 销毁旧服务端，不能依赖旧内存记录。
        QTRY_VERIFY(!keHuA.online() && !keHuB.online()); // 客户端感知断线。
        kaiFang = std::make_unique<MessageServer>(QList<DeviceCredential>{}); // 创建全新的服务端对象。
        QVERIFY(kaiFang->qiYongZhuCe(jiLu)); // 从磁盘恢复两台已登记身份。
        QVERIFY(kaiFang->listen(QHostAddress::LocalHost, duanKou)); // 恢复监听。
        QTRY_VERIFY_WITH_TIMEOUT(keHuA.online() && keHuB.online(), 6000); // 原身份重连登记成功且不重复创建。
        QCOMPARE(keHuA.profile().deviceId, a.deviceId); // 客户端身份没有因重启改变。
        SheBeiZhuCe huiFu; // 独立读取持久记录检查是否重复注册。
        QVERIFY(huiFu.duQu({}, jiLu, &cuoWu)); // 读取服务端保存的实际文件。
        QCOMPARE(huiFu.sheBei().size(), 2); // 重连只恢复原两台设备。
    }

    // 验证不可达主入口自动转向备用服务器，并把成功地址保存为下次首选。
    void ziDongQieHuanRuKou() // 用已关闭的本机端口替代不可达内网。
    {
        QTemporaryDir muLu; // 隔离身份保存文件。
        QVERIFY(muLu.isValid()); // 临时目录可以写入。
        MessageServer kaiFang({}); // 本机备用入口支持自动注册。
        QVERIFY(kaiFang.qiYongZhuCe(muLu.filePath("devices.json"))); // 保存登记结果。
        QVERIFY(kaiFang.listen(QHostAddress::LocalHost, 0)); // 自动分配备用入口端口。
        QTcpServer kongDuanKou; // 获取一个未运行服务的端口。
        QVERIFY(kongDuanKou.listen(QHostAddress::LocalHost, 0)); // 先占用端口确认与真实服务端不同。
        const QUrl shiBai(QStringLiteral("ws://127.0.0.1:%1").arg(kongDuanKou.serverPort())); // 模拟第一个入口。
        kongDuanKou.close(); // 关闭监听后该入口会立即拒绝连接。
        const QUrl chengGong(QStringLiteral("ws://127.0.0.1:%1").arg(kaiFang.port())); // 第二个入口可正常注册。
        ClientProfile peiZhi; QString cuoWu; // 模拟首次运行的独立电脑。
        QVERIFY(KeHuDuanPeiZhi::duQuZiDong(muLu.filePath("client.json"), &peiZhi, &cuoWu)); // 自动准备身份。
        peiZhi.server = shiBai; peiZhi.beiYongDiZhi = {chengGong}; // 仅尝试两个本机地址。
        MessageClient keHuDuan; // 使用生产网络重试逻辑。
        keHuDuan.start(peiZhi); // 首次尝试会失败，之后切换。
        QTRY_VERIFY_WITH_TIMEOUT(keHuDuan.online(), 5000); // 自动切换必须实际完成认证。
        QCOMPARE(keHuDuan.profile().server, chengGong); // 当前地址确实是备用入口。
        ClientProfile huiFu; // 模拟随后重新打开客户端。
        QVERIFY(KeHuDuanPeiZhi::duQuZiDong(peiZhi.peiZhiLuJing, &huiFu, &cuoWu)); // 读取成功认证时保存的文件。
        QCOMPARE(huiFu.server, chengGong); // 下次先尝试成功地址。
        QCOMPARE(huiFu.deviceId, peiZhi.deviceId); // 地址切换不会重新注册新身份。
    }

    // 验证旧令牌冲突时自动生成新身份，同时绝不覆盖服务端已有身份。
    void ziDongHuiFuChongTuShenFen() // 本次连接最多恢复一次身份，避免无限创建。
    {
        QTemporaryDir muLu; // 所有身份文件与真实部署隔离。
        QVERIFY(muLu.isValid()); // 检查临时目录可用。
        MessageServer kaiFang({{"device-a", QStringLiteral("原设备"), tokenA}}); // 服务端已有固定身份。
        const auto jiLu = muLu.filePath("devices.json"); // 新设备记录独立保存。
        QVERIFY(kaiFang.qiYongZhuCe(jiLu)); // 开放登记，但不放宽原身份校验。
        QVERIFY(kaiFang.listen(QHostAddress::LocalHost, 0)); // 监听本机测试入口。
        ClientProfile peiZhi{QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(kaiFang.port())), "device-a", tokenB}; // 模拟缓存了不匹配的旧令牌。
        peiZhi.sheBeiMing = QStringLiteral("新电脑"); // 提供自动登记所需的设备名称。
        peiZhi.peiZhiLuJing = muLu.filePath("client.json"); // 新身份必须先永久保存。
        MessageClient keHuDuan; // 实际客户端处理服务端 auth_failed。
        keHuDuan.start(peiZhi); // 首次登记冲突，然后新建自己的 UUID。
        QTRY_VERIFY_WITH_TIMEOUT(keHuDuan.online(), 5000); // 新身份最终允许加入聊天室。
        QVERIFY(keHuDuan.profile().deviceId != "device-a"); // 不能继续冒用原 ID。
        SheBeiZhuCe huiFu; QString cuoWu; // 重新检查固定身份与动态记录。
        QVERIFY(huiFu.duQu({{"device-a", QStringLiteral("原设备"), tokenA}}, jiLu, &cuoWu)); // 合并原配置和注册文件。
        QVERIFY(huiFu.yanZheng("device-a", tokenA)); // 原设备令牌未被覆盖。
        QVERIFY(!huiFu.yanZheng("device-a", tokenB)); // 错误旧令牌仍被拒绝。
        QCOMPARE(huiFu.sheBei().size(), 2); // 仅新增一台独立设备。
    }

    // 验证身份恢复和认证成功后的本机保存失败都会停止连接，并保留具体文件错误。
    void ziDongShenFenBaoCunShiBai() // 用文件阻挡目录模拟错误，不改动真实电脑权限。
    {
        QTemporaryDir muLu; // 隔离服务端注册记录和客户端配置。
        QVERIFY(muLu.isValid()); // 后续文件必须在临时目录中创建。
        QFile zuDang(muLu.filePath("not-a-directory")); // 普通文件不能作为客户端身份目录。
        QVERIFY(zuDang.open(QIODevice::WriteOnly)); // 创建可靠的保存失败条件。
        zuDang.close(); // 释放句柄，避免测试依赖 Windows 的共享锁行为。
        MessageServer kaiFang({{"device-a", QStringLiteral("原设备"), tokenA}}); // 保留一台固定设备用于制造身份冲突。
        QVERIFY(kaiFang.qiYongZhuCe(muLu.filePath("devices.json"))); // 服务端身份库本身可以正常写入。
        QVERIFY(kaiFang.listen(QHostAddress::LocalHost, 0)); // 只监听本机测试端口。
        ClientProfile peiZhi{QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(kaiFang.port())), "device-a", tokenB}; // 错误令牌会触发自动身份恢复。
        peiZhi.sheBeiMing = QStringLiteral("保存失败测试"); // 使用统一自动登记流程。
        peiZhi.peiZhiLuJing = zuDang.fileName() + "/client.json"; // 恢复时必须先写入这个不可用路径。
        MessageClient chongTu; // 第一种失败发生在创建新身份阶段。
        QSignalSpy chongTuCuoWu(&chongTu, &MessageClient::problem); // 记录最后实际展示给用户的提示。
        chongTu.start(peiZhi); // 原 ID 不可覆盖，新 ID 也不能保存。
        QTRY_VERIFY(!chongTu.active()); // 文件失败必须停止，不能无限尝试登记。
        QVERIFY(!chongTu.online()); // 未保存的新身份不能宣布上线。
        QVERIFY(!chongTuCuoWu.isEmpty()); // 错误必须可见。
        QVERIFY(chongTuCuoWu.last().first().toString().contains(QStringLiteral("身份目录"))); // 文件原因不能被令牌错误覆盖。
        QCOMPARE(chongTu.profile().deviceId, QStringLiteral("device-a")); // 写失败不能替换内存中的原身份。
        peiZhi.deviceId = QStringLiteral("new-device"); peiZhi.token = tokenC; // 第二种场景使用服务端能够认可的新身份。
        MessageClient dengJi; // 注册成功后的入口保存仍会失败。
        QSignalSpy dengJiCuoWu(&dengJi, &MessageClient::problem); // 检查认证完成阶段的具体错误。
        dengJi.start(peiZhi); // 服务端写入成功，但本机配置无法写入。
        QTRY_VERIFY(!dengJi.active()); // 必须停止连接，不能声称已经完成准备。
        QVERIFY(!dengJi.online()); // 不留下界面显示正常的半完成状态。
        QVERIFY(!dengJiCuoWu.isEmpty()); // 用户仍然能得到具体错误。
        QVERIFY(dengJiCuoWu.last().first().toString().contains(QStringLiteral("身份目录"))); // 正确定位到本机文件问题。
    }

    // 验证长期注册的离线身份不会挤占在线列表，32 个最长特殊名称也不会使成员消息超限断线。
    void zhuCeChengYuanLieBiaoShangXian() // 通过真实连接验证字节上限和在线容量一起成立。
    {
        QTemporaryDir muLu; // 测试不使用真实服务端身份库。
        QVERIFY(muLu.isValid()); // 确保隔离目录可写。
        const auto jiLu = muLu.filePath("devices.json"); // 预先登记的身份保存在临时文件中。
        SheBeiZhuCe zhuCe; QString cuoWu; // 模拟累计注册超过在线容量的设备库。
        QVERIFY(zhuCe.duQu({}, jiLu, &cuoWu)); // 没有固定名单也可以开放登记。
        QList<DeviceCredential> pingJu; // 留存测试登录需要的各自凭据。
        for (int i = 0; i < 40; ++i) { // 40 个登记身份中只有前 32 个同时在线。
            const DeviceCredential xin{QString(76, 'd') + QString::number(i), // 接近最长合法 ID，覆盖成员消息的字节压力。
                QStringLiteral("名") + QString(79, QChar(1)), // 控制字符在 JSON 中会转义为六个字节，名称长度仍然合法。
                QString(40, 't') + QString::number(i)}; // 每个身份具有独立令牌。
            QVERIFY(zhuCe.dengJi(xin).isEmpty()); // 先永久登记，避免测试依赖服务端内存残留。
            pingJu.append(xin); // 保存稍后登录使用的参数。
        }
        MessageServer kaiFang({}); // 服务端读取之前保存的全部 40 个身份。
        QVERIFY(kaiFang.qiYongZhuCe(jiLu)); // 恢复全部动态设备。
        QVERIFY(kaiFang.listen(QHostAddress::LocalHost, 0)); // 本机端口不影响真实服务端。
        const QUrl ruKou(QStringLiteral("ws://127.0.0.1:%1").arg(kaiFang.port())); // 所有测试连接使用相同入口。
        std::vector<std::unique_ptr<Probe>> lianJie; // 每个探针拥有一个独立且不可复制的 WebSocket。
        for (int i = 0; i < 32; ++i) { // 达到允许的最大同时连接数。
            auto keHu = std::make_unique<Probe>(); // 创建当前身份的独立连接。
            keHu->socket.setMaxAllowedIncomingMessageSize(protocol::maxWireBytes); // 与真实客户端执行相同的接收上限。
            keHu->socket.open(ruKou); // 发起本机连接。
            QTRY_COMPARE(keHu->socket.state(), QAbstractSocket::ConnectedState); // 登录请求必须等待握手完成。
            keHu->send(protocol::message("auth.login", {{"device_id", pingJu[i].id}, {"token", pingJu[i].token}})); // 用恢复的动态身份登录。
            QTRY_VERIFY(keHu->has("auth.result")); // 每个合法身份都应成功上线。
            lianJie.push_back(std::move(keHu)); // 保持此前连接存活，累积到 32 台同时在线。
        }
        for (const auto& keHu : lianJie) { // 检查所有客户端收到最终完整名单且保持连接。
            QTRY_COMPARE(keHu->last("device.list").value("payload").toObject().value("devices").toArray().size(), 32); // 8 个离线历史身份不占名单位置。
            QCOMPARE(keHu->socket.state(), QAbstractSocket::ConnectedState); // 特殊名称不能造成协议超限断线。
            QJsonObject jieXi; // 用真实协议解析器检查最终广播，而非仅比较构造函数的输出。
            QVERIFY(protocol::decode(protocol::encode(keHu->last("device.list")), &jieXi)); // 证明整个消息在协议允许的字节范围内。
        }
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
