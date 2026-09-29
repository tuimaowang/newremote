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
#include <QNetworkInterface>
#include <QWebSocketServer>
#include <cstring>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTextStream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Use the shared product name so the server and client resolve the same
    // automatic configuration directory under the user's app-data folder.
    app.setApplicationName("FSRemoteMessages");
    app.setOrganizationName("FSRemote");
    app.setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.setApplicationDescription("Minimal authenticated device-message relay. No remote commands.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"config", "Server JSON configuration file.", "path"});
    parser.addOption({"lan", "Generate and serve an automatic plain-WebSocket LAN test configuration."});
    parser.process(app);
    QTextStream output(stdout);
    QTextStream error(stderr);
    const bool automatic = !parser.isSet("config");
    const bool lanMode = parser.isSet("lan");
    QString configPath = parser.value("config");
    if (automatic) {
        const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (directory.isEmpty() || !QDir().mkpath(directory)) {
            error << "Cannot create automatic configuration directory.\n";
            return 1;
        }
        configPath = QDir(directory).filePath(lanMode ? "lan-server.json" : "server.json");
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
                {"listen", lanMode ? "0.0.0.0" : "127.0.0.1"}, {"port", 0},
                {"allow_insecure_lan", lanMode},
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
    const QHostAddress address(config.value("listen").toString("127.0.0.1"));
    const auto portValue = config.value("port").toDouble(automatic ? 0 : 17890);
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
    if (automatic) {
        const auto directory = QFileInfo(configPath).absoluteDir();
        QString clientHost = QStringLiteral("127.0.0.1");
        if (lanMode || allowInsecureLan) {
            for (const auto& interface : QNetworkInterface::allInterfaces()) {
                if (!(interface.flags() & QNetworkInterface::IsUp)
                    || !(interface.flags() & QNetworkInterface::IsRunning)
                    || (interface.flags() & QNetworkInterface::IsLoopBack)) continue;
                for (const auto& entry : interface.addressEntries()) {
                    const auto candidate = entry.ip();
                    if (candidate.protocol() != QAbstractSocket::IPv4Protocol
                        || candidate.isLoopback() || candidate.toString().startsWith("169.254.")) continue;
                    clientHost = candidate.toString();
                    break;
                }
                if (clientHost != QStringLiteral("127.0.0.1")) break;
            }
        }
        const auto serverUrl = QStringLiteral("%1://%2:%3")
            .arg(secure ? QStringLiteral("wss") : QStringLiteral("ws"), clientHost)
            .arg(server.port());
        const auto devices = config.value("devices").toArray();
        for (const auto& value : devices) {
            const auto device = value.toObject();
            const auto id = device.value("id").toString();
            if (id.isEmpty()) continue;
            QSaveFile clientFile(directory.filePath(QStringLiteral("client-%1.json").arg(id)));
            if (!clientFile.open(QIODevice::WriteOnly)) continue;
            const QJsonObject client{{"server", serverUrl}, {"device_id", id},
                                     {"token", device.value("token").toString()},
                                     {"allow_insecure_lan", allowInsecureLan}};
            clientFile.write(QJsonDocument(client).toJson(QJsonDocument::Indented));
            clientFile.commit();
        }
        output << "Automatic configuration: " << configPath << Qt::endl;
        if (allowInsecureLan) {
            output << "LAN test mode: plain WebSocket is enabled explicitly; use only on a trusted private network." << Qt::endl;
            output << "Copy client-device-*.json beside FSRemoteMessages.exe on each test computer." << Qt::endl;
        } else {
            output << "Automatic mode is local-only (127.0.0.1); configure WSS for LAN clients." << Qt::endl;
        }
    }
    output << "Listening " << (secure ? "wss://" : "ws://") << address.toString()
           << ':' << server.port() << Qt::endl;
    return app.exec();
}
