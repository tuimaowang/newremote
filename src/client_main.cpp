#include "main_window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QTimer>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("FSRemoteMessages");
    app.setOrganizationName("FSRemote");
    app.setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"config", "Client JSON configuration file; connects automatically.", "path"});
    parser.process(app);
    ClientProfile profile;
    QString configPath = parser.value("config");
    const bool automatic = !parser.isSet("config");
    if (automatic) {
        const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        configPath = QDir(directory).filePath("client-device-a.json"); // 默认读取服务器生成的公网实际配置。
        if (!QFile::exists(configPath)) {
            const auto besideExecutable = QDir(QCoreApplication::applicationDirPath())
                .filePath("client-device-a.json"); // 也支持从程序目录直接复制服务器生成的配置。
            if (QFile::exists(besideExecutable)) configPath = besideExecutable;
        }
    }
    if (parser.isSet("config") || QFile::exists(configPath)) {
        QFile file(configPath);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) {
            QMessageBox::critical(nullptr, QStringLiteral("配置错误"), automatic
                ? QStringLiteral("无法读取自动配置，请先启动服务器")
                : QStringLiteral("无法读取客户端配置文件"));
            return 1;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            QMessageBox::critical(nullptr, QStringLiteral("配置错误"), QStringLiteral("配置文件不是有效的 JSON 对象"));
            return 1;
        }
        const auto config = document.object();
        profile = {QUrl(config.value("server").toString()), config.value("device_id").toString(),
                   config.value("token").toString(), config.value("allow_insecure_lan").toBool()};
        const auto validation = MessageClient::validate(profile);
        if (!validation.isEmpty()) {
            QMessageBox::critical(nullptr, QStringLiteral("配置错误"), validation);
            return 1;
        }
    }
    if (automatic && !QFile::exists(configPath)) {
        QMessageBox::information(nullptr, QStringLiteral("等待服务器配置"),
            QStringLiteral("请先启动 FSRemoteMessageServer。服务器会自动生成连接配置，然后重新打开本程序。"));
        return 0;
    }
    MainWindow window(profile);
    window.show();
    if (parser.isSet("config") || automatic) QTimer::singleShot(0, &window, &MainWindow::connectToServer);
    return app.exec();
}
