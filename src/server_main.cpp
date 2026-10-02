#include "message_server.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QSaveFile>
#include <QWebSocketServer>
#include <cstring>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTextStream>

// 启动消息服务端；自动模式迁移配置时先释放读取句柄，保存失败则报告具体原因并退出。
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Use the shared product name so the server and client resolve the same
    // automatic configuration directory under the user's app-data folder.
    app.setApplicationName("FSRemoteMessages");
    app.setOrganizationName("FSRemote");
    app.setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("经过身份验证的设备消息中继服务，不执行远程命令。"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"config", QStringLiteral("服务器 JSON 配置文件路径。"), QStringLiteral("路径")});
    parser.process(app);
    QTextStream output(stdout);
    QTextStream error(stderr);
    const bool automatic = !parser.isSet("config");
    QString configPath = parser.value("config");
    if (automatic) {
        const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (directory.isEmpty() || !QDir().mkpath(directory)) {
            error << "Cannot create automatic configuration directory.\n";
            return 1;
        }
        configPath = QDir(directory).filePath("server.json"); // 实际部署只使用一个固定服务器配置。
        if (!QFile::exists(configPath)) {
            auto token = [] {
                QByteArray bytes(32, Qt::Uninitialized);
                auto* random = QRandomGenerator::system();
                for (int offset = 0; offset < bytes.size(); offset += 4) {
                    const auto value = random->generate();
                    std::memcpy(bytes.data() + offset, &value, sizeof(value));
                }
                return QString::fromLatin1(bytes.toBase64());
            };
            const QJsonObject generated{
                {"listen", "0.0.0.0"}, {"port", 62843}, // 固定 iKuai 映射端口，避免客户端拿到随机本机端口。
                {"allow_insecure_lan", true}, // 当前公网测试仍使用明文 WS，正式部署再切换 WSS。
                {"lan_address", "192.168.3.63"}, // 生成局域网客户端配置使用的服务器地址。
                {"public_address", "112.26.74.220"}, // 生成公网客户端配置使用的服务器地址。
                {"devices", QJsonArray{
                    QJsonObject{{"id", "device-a"}, {"name", "本机 A"}, {"token", token()}},
                    QJsonObject{{"id", "device-b"}, {"name", "本机 B"}, {"token", token()}}
                }}
            };
            QSaveFile file(configPath);
            if (!file.open(QIODevice::WriteOnly)
                || file.write(QJsonDocument(generated).toJson(QJsonDocument::Indented)) < 0
                || !file.commit()) {
                error << "Cannot write automatic configuration.\n";
                return 1;
            }
        }
        QFile existing(configPath); // 检查旧版随机端口配置，避免继续使用 127.0.0.1。
        if (existing.open(QIODevice::ReadOnly)) { // 旧配置存在时保留设备令牌并迁移部署字段。
            QJsonParseError oldError; // 保存旧配置解析结果。
            const auto oldDocument = QJsonDocument::fromJson(existing.readAll(), &oldError); // 读取旧配置内容。
            existing.close(); // Windows 下原子替换前必须关闭旧文件，避免读取句柄阻止重命名。
            if (oldError.error == QJsonParseError::NoError && oldDocument.isObject()) { // 只有有效对象才执行迁移。
                auto migrated = oldDocument.object(); // 复制旧设备列表和其他已知字段。
                migrated.insert("listen", "0.0.0.0"); // 服务器必须接受局域网和公网连接。
                migrated.insert("port", 62843); // 统一使用路由器映射的固定端口。
                migrated.insert("allow_insecure_lan", true); // 兼容当前明文测试链路。
                migrated.insert("lan_address", migrated.value("lan_address").toString("192.168.3.63")); // 补齐局域网地址。
                migrated.insert("public_address", migrated.value("public_address").toString("112.26.74.220")); // 补齐公网地址。
                QSaveFile file(configPath); // 以原子方式写回迁移后的配置。
                if (!file.open(QIODevice::WriteOnly)
                    || file.write(QJsonDocument(migrated).toJson(QJsonDocument::Indented)) < 0
                    || !file.commit()) { // 迁移失败时阻止服务端继续使用不明确配置。
                    error << "Cannot migrate automatic configuration: " << configPath
                          << ": " << file.errorString() << '\n'; // 输出配置路径及保存错误，区分占用和权限问题。
                    return 1; // 保存失败时退出，避免继续使用未迁移的旧配置。
                }
            }
        }
    }
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
        error << "Cannot read configuration (maximum 64 KiB). Use --config <path>.\n";
        return 1;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error << "Invalid configuration JSON.\n"; return 1;
    }
    const auto config = document.object();
    const QHostAddress address(config.value("listen").toString("0.0.0.0")); // 默认绑定所有网卡。
    const auto portValue = config.value("port").toDouble(automatic ? 62843 : 17890); // 自动模式固定公网端口。
    const bool allowInsecureLan = config.value("allow_insecure_lan").toBool(false);
    if (address.isNull() || portValue < 0 || portValue > 65535 || portValue != int(portValue)) {
        error << "Invalid listen address or port.\n"; return 1;
    }
    QList<DeviceCredential> credentials;
    for (const auto& value : config.value("devices").toArray()) {
        const auto device = value.toObject();
        credentials.append({device.value("id").toString(), device.value("name").toString(),
                            device.value("token").toString()});
    }
    const auto certificatePath = config.value("certificate").toString();
    const auto keyPath = config.value("private_key").toString();
    QSslConfiguration tls;
    const bool secure = !certificatePath.isEmpty() || !keyPath.isEmpty();
    if (secure) {
        const auto directory = QFileInfo(file).absoluteDir();
        QFile certificateFile(directory.filePath(certificatePath));
        QFile keyFile(directory.filePath(keyPath));
        if (certificatePath.isEmpty() || keyPath.isEmpty() || !QSslSocket::supportsSsl()
            || !certificateFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) {
            error << "Cannot load TLS certificate/key or TLS backend unavailable.\n"; return 1;
        }
        const auto certificates = QSslCertificate::fromData(certificateFile.readAll());
        const auto keyData = keyFile.readAll();
        auto key = QSslKey(keyData, QSsl::Rsa);
        if (key.isNull()) key = QSslKey(keyData, QSsl::Ec);
        if (certificates.isEmpty() || key.isNull()) {
            error << "Invalid PEM certificate or unencrypted RSA/EC private key.\n"; return 1;
        }
        tls = QSslConfiguration::defaultConfiguration();
        tls.setLocalCertificateChain(certificates);
        tls.setPrivateKey(key);
        tls.setProtocol(QSsl::TlsV1_2OrLater);
        tls.setPeerVerifyMode(QSslSocket::VerifyNone);
    }
    MessageServer server(credentials);
    QObject::connect(&server, &MessageServer::activity, &app, [&output](const QString& message) {
        output << message << Qt::endl;
    });
    if (!server.listen(address, static_cast<quint16>(portValue), secure ? &tls : nullptr,
                       allowInsecureLan)) {
        error << server.errorString() << '\n'; return 1;
    }
    if (automatic) { // 自动生成实际部署所需的局域网和公网客户端配置。
        const auto directory = QFileInfo(configPath).absoluteDir();
        const auto lanHost = config.value("lan_address").toString("192.168.3.63"); // 读取固定局域网入口。
        const auto publicHost = config.value("public_address").toString("112.26.74.220"); // 读取固定公网入口。
        const auto devices = config.value("devices").toArray();
        for (const auto& value : devices) {
            const auto device = value.toObject();
            const auto id = device.value("id").toString();
            if (id.isEmpty()) continue;
            const auto writeClient = [&](const QString& host, const QString& suffix) { // 为一个设备生成指定网络入口配置。
                QSaveFile clientFile(directory.filePath(QStringLiteral("client-%1%2.json").arg(id, suffix))); // 输出可直接复制的配置文件。
                if (!clientFile.open(QIODevice::WriteOnly)) return false; // 无法写入时让调用方记录失败。
                const auto serverUrl = QStringLiteral("%1://%2:%3")
                    .arg(secure ? QStringLiteral("wss") : QStringLiteral("ws"), host)
                    .arg(server.port()); // 使用实际监听端口生成客户端入口。
                const QJsonObject client{{"server", serverUrl}, {"device_id", id},
                                         {"token", device.value("token").toString()},
                                         {"allow_insecure_lan", allowInsecureLan}}; // 每台设备保留独立令牌。
                return clientFile.write(QJsonDocument(client).toJson(QJsonDocument::Indented)) >= 0
                    && clientFile.commit(); // 原子写入，避免复制半个配置文件。
            };
            if (!writeClient(publicHost, QStringLiteral(""))
                || !writeClient(lanHost, QStringLiteral("-lan"))) { // 公网默认文件和局域网备用文件都必须生成。
                error << "Cannot write client configuration for " << id << ".\n"; return 1;
            }
        }
        output << "Automatic configuration: " << configPath << Qt::endl;
        output << "Copy client-device-*.json for public access, or client-device-*-lan.json for LAN access." << Qt::endl; // 明确两种实际部署入口。
    }
    output << "Listening " << (secure ? "wss://" : "ws://") << address.toString()
           << ':' << server.port() << Qt::endl;
    return app.exec();
}
