#include "message_server.h" // 引入真正负责 WebSocket 连接、认证和消息转发的服务类。
#include <QCommandLineParser> // 提供 --help、--version 和 --config 等命令行参数解析。
#include <QCoreApplication> // 提供无图形界面程序的 Qt 事件循环。
#include <QDir> // 提供目录创建、路径拼接等文件系统操作。
#include <QFile> // 提供普通文件的读取能力。
#include <QFileInfo> // 提供文件的绝对路径和所在目录信息。
#include <QJsonDocument> // 在 JSON 文本和 Qt JSON 对象之间转换。
#include <QJsonObject> // 表示 JSON 对象，例如 server.json 的配置主体。
#include <QJsonArray> // 表示 JSON 数组，例如 devices 设备列表。
#include <QRandomGenerator> // 生成设备令牌所需的随机数据。
#include <QStandardPaths> // 获取当前用户的标准应用数据目录。
#include <QSaveFile> // 以临时文件加原子替换的方式安全保存配置。
#include <QWebSocketServer> // 提供 WebSocket 服务端相关类型和链接依赖。
#include <cstring> // 提供 memcpy，用于复制随机数到令牌缓冲区。
#include <QSslCertificate> // 读取和解析 PEM 格式 TLS 证书。
#include <QSslKey> // 读取和解析 RSA 或 EC 私钥。
#include <QSslSocket> // 检查 Qt TLS 能力并提供 TLS 版本常量。
#include <QTextStream> // 向标准输出和标准错误输出文本。

// 启动消息服务端；自动模式迁移配置时先释放读取句柄，保存失败则报告具体原因并退出。
int main(int argc, char** argv)
{
    // 创建无窗口的 Qt 应用对象；它负责初始化 Qt，并在末尾提供网络事件循环。
    QCoreApplication app(argc, argv);
    // 服务端和客户端必须使用相同的组织名与应用名，才能定位同一个用户配置目录。
    app.setApplicationName("FSRemoteMessages"); // 设置应用名，参与 AppDataLocation 路径计算。
    app.setOrganizationName("FSRemote"); // 设置组织名，参与 AppDataLocation 路径计算。
    app.setApplicationVersion("0.1.0"); // 设置 --version 输出的版本号。
    // 创建命令行解析器；它只负责解析参数，不负责执行参数对应的业务逻辑。
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("经过身份验证的设备消息中继服务，不执行远程命令。")); // 设置 --help 中的程序说明。
    parser.addHelpOption(); // 注册 --help，并由 Qt 自动生成帮助文本。
    parser.addVersionOption(); // 注册 --version，并显示前面设置的应用版本。
    parser.addOption({"config", QStringLiteral("服务器 JSON 配置文件路径。"), QStringLiteral("路径")}); // 注册可选配置路径。
    parser.process(app); // 读取 argc/argv，并让解析器准备好后续 isSet/value 查询。
    QTextStream output(stdout); // 创建标准输出流，用于输出启动状态和活动日志。
    QTextStream error(stderr); // 创建标准错误流，用于输出错误信息。
    const bool automatic = !parser.isSet("config"); // 未指定 --config 时进入自动配置模式。
    QString configPath = parser.value("config"); // 读取 --config 的参数；未指定时结果为空。

    // 自动模式使用固定的用户应用数据目录，并负责生成或迁移 server.json。
    if (automatic) {
        // Qt 会根据组织名和应用名返回当前用户可写的应用数据目录。
        const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        // 目录为空表示系统没有提供有效位置；mkpath 会递归创建不存在的目录。
        if (directory.isEmpty() || !QDir().mkpath(directory)) {
            error << QStringLiteral("无法创建自动配置目录。\n"); // 使用中文提示说明自动配置目录创建失败。
            return 1;
        }
        // 将固定文件名拼接到应用数据目录，得到服务端实际使用的配置路径。
        configPath = QDir(directory).filePath("server.json"); // 实际部署只使用一个固定服务器配置。

        // 首次启动没有配置时，先生成默认服务端配置和两个设备身份。
        if (!QFile::exists(configPath)) {
            // 生成一个独立设备令牌；令牌会写入配置并用于客户端身份认证。
            auto token = [] {
                QByteArray bytes(32, Qt::Uninitialized); // 分配 32 字节未初始化缓冲区作为随机令牌原始数据。
                auto* random = QRandomGenerator::system(); // 获取 Qt 提供的系统随机数生成器。
                // 每次生成 4 字节随机数，正好填满 32 字节缓冲区。
                for (int offset = 0; offset < bytes.size(); offset += 4) {
                    const auto value = random->generate(); // 生成一个 32 位随机值。
                    std::memcpy(bytes.data() + offset, &value, sizeof(value)); // 将随机值复制到令牌缓冲区。
                }
                return QString::fromLatin1(bytes.toBase64()); // Base64 便于把二进制令牌安全写入 JSON 文本。
            };
            // 组装首次启动使用的 JSON 配置对象；字段名属于配置协议，不能随意翻译。
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
            // QSaveFile 先写临时文件，commit 成功后才替换目标文件，避免留下半份 JSON。
            QSaveFile file(configPath);
            if (!file.open(QIODevice::WriteOnly)
                || file.write(QJsonDocument(generated).toJson(QJsonDocument::Indented)) < 0
                || !file.commit()) {
                error << QStringLiteral("无法写入自动配置。\n"); // 使用中文提示说明首次生成配置失败。
                return 1;
            }
        }
        // 读取现有配置，用于把旧版随机端口或本机地址迁移到当前部署配置。
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
                    error << QStringLiteral("无法迁移自动配置：") << configPath
                          << QStringLiteral("：") << file.errorString() << '\n'; // 使用中文前缀并保留系统底层错误详情。
                    return 1; // 保存失败时退出，避免继续使用未迁移的旧配置。
                }
            }
        }
    }
    // 无论是自动模式还是显式 --config，下面统一读取最终配置。
    QFile file(configPath);
    // 限制配置大小，避免错误路径或异常文件被当作配置无限读取。
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
        error << QStringLiteral("无法读取配置（最大 64 KiB）。请使用 --config <路径>。\n"); // 使用中文提示说明读取限制和备用参数。
        return 1;
    }
    QJsonParseError parseError; // 保存 JSON 解析失败位置和错误类型。
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError); // 将配置文件文本解析为 JSON 文档。
    // 服务端只接受 JSON 对象作为根节点，例如 { "listen": "...", "devices": [...] }。
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error << QStringLiteral("配置 JSON 无效。\n"); return 1; // 使用中文提示说明配置格式解析失败。
    }
    const auto config = document.object(); // 取出根 JSON 对象，后续按字段读取配置。

    // 读取监听地址、端口和明文局域网策略，转换成服务端 API 所需的 Qt 类型。
    const QHostAddress address(config.value("listen").toString("0.0.0.0")); // 默认绑定所有网卡。
    const auto portValue = config.value("port").toDouble(automatic ? 62843 : 17890); // 自动模式固定公网端口。
    const bool allowInsecureLan = config.value("allow_insecure_lan").toBool(false);
    // 地址必须能解析，端口必须是 0 到 65535 的整数；否则启动没有明确的监听目标。
    if (address.isNull() || portValue < 0 || portValue > 65535 || portValue != int(portValue)) {
        error << QStringLiteral("监听地址或端口无效。\n"); return 1; // 使用中文提示说明网络监听参数不合法。
    }

    // 将 JSON 中的 devices 数组转换成 MessageServer 使用的设备凭据列表。
    QList<DeviceCredential> credentials;
    for (const auto& value : config.value("devices").toArray()) {
        const auto device = value.toObject(); // 将数组中的一个 JSON 值转换为设备对象。
        credentials.append({device.value("id").toString(), device.value("name").toString(),
                            device.value("token").toString()}); // 保存设备 ID、显示名和认证令牌。
    }

    // certificate/private_key 同时存在时启用 WSS；两个字段都为空时使用普通 WS。
    const auto certificatePath = config.value("certificate").toString();
    const auto keyPath = config.value("private_key").toString();
    QSslConfiguration tls; // 保存后续传给 MessageServer 的 TLS 配置。
    const bool secure = !certificatePath.isEmpty() || !keyPath.isEmpty(); // 任一 TLS 文件被指定就进入加密配置流程。
    if (secure) {
        // 证书路径按配置文件所在目录解析，而不是按当前 PowerShell 目录解析。
        const auto directory = QFileInfo(file).absoluteDir();
        QFile certificateFile(directory.filePath(certificatePath)); // 打开服务端证书文件。
        QFile keyFile(directory.filePath(keyPath)); // 打开服务端私钥文件。
        // TLS 必须同时具备证书、私钥、Qt TLS 后端和可读文件。
        if (certificatePath.isEmpty() || keyPath.isEmpty() || !QSslSocket::supportsSsl()
            || !certificateFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) {
            error << QStringLiteral("无法加载 TLS 证书或私钥，或 TLS 后端不可用。\n"); return 1; // 使用中文提示说明 TLS 初始化失败。
        }
        const auto certificates = QSslCertificate::fromData(certificateFile.readAll()); // 解析 PEM 证书链。
        const auto keyData = keyFile.readAll(); // 读取私钥原始内容。
        auto key = QSslKey(keyData, QSsl::Rsa); // 先按 RSA 私钥尝试解析。
        if (key.isNull()) key = QSslKey(keyData, QSsl::Ec); // RSA 失败时再按 EC 私钥尝试。
        // 证书为空或私钥无法解析时，不能启动一个看似安全但实际无效的 WSS 服务。
        if (certificates.isEmpty() || key.isNull()) {
            error << QStringLiteral("PEM 证书无效，或私钥不是未加密的 RSA/EC 私钥。\n"); return 1; // 使用中文提示说明证书或私钥格式错误。
        }
        tls = QSslConfiguration::defaultConfiguration(); // 从 Qt 默认 TLS 参数开始构造服务端配置。
        tls.setLocalCertificateChain(certificates); // 设置服务端向客户端出示的证书链。
        tls.setPrivateKey(key); // 设置与证书匹配的私钥。
        tls.setProtocol(QSsl::TlsV1_2OrLater); // 只允许 TLS 1.2 及更新协议。
        tls.setPeerVerifyMode(QSslSocket::VerifyNone); // 不要求客户端提供证书，设备令牌负责客户端认证。
    }
    // 创建消息服务对象；credentials 会用于校验设备连接时提交的身份令牌。
    MessageServer server(credentials);
    // 将 MessageServer 的活动信号连接到标准输出，实时显示连接和消息日志。
    QObject::connect(&server, &MessageServer::activity, &app, [&output](const QString& message) {
        output << message << Qt::endl; // 写入日志并立即刷新，便于在控制台实时观察服务状态。
    });
    // 开始监听；secure 决定是否传入 TLS 配置，allowInsecureLan 控制明文局域网策略。
    if (!server.listen(address, static_cast<quint16>(portValue), secure ? &tls : nullptr,
                       allowInsecureLan)) {
        error << QStringLiteral("监听失败：") << server.errorString() << '\n'; return 1; // 使用中文前缀并保留 Qt 提供的底层错误详情。
    }
    // 自动模式在服务端成功监听后生成客户端配置，确保配置中的端口与实际监听端口一致。
    if (automatic) { // 自动生成实际部署所需的局域网和公网客户端配置。
        const auto directory = QFileInfo(configPath).absoluteDir();
        const auto lanHost = config.value("lan_address").toString("192.168.3.63"); // 读取固定局域网入口。
        const auto publicHost = config.value("public_address").toString("112.26.74.220"); // 读取固定公网入口。
        const auto devices = config.value("devices").toArray();
        // 每台设备生成一份公网配置和一份局域网配置，令牌与设备一一对应。
        for (const auto& value : devices) {
            const auto device = value.toObject();
            const auto id = device.value("id").toString();
            if (id.isEmpty()) continue; // 没有设备 ID 就无法生成可用客户端配置，跳过该条记录。
            const auto writeClient = [&](const QString& host, const QString& suffix) { // 为一个设备生成指定网络入口配置。
                // 根据设备 ID 和后缀区分公网文件与局域网文件。
                QSaveFile clientFile(directory.filePath(QStringLiteral("client-%1%2.json").arg(id, suffix))); // 输出可直接复制的配置文件。
                if (!clientFile.open(QIODevice::WriteOnly)) return false; // 无法写入时让调用方记录失败。
                // 根据是否启用 TLS 选择 ws 或 wss，并使用 server.port() 获取实际端口。
                const auto serverUrl = QStringLiteral("%1://%2:%3")
                    .arg(secure ? QStringLiteral("wss") : QStringLiteral("ws"), host)
                    .arg(server.port()); // 使用实际监听端口生成客户端入口。
                // 客户端只需服务器地址、设备 ID、对应令牌和明文局域网策略即可连接。
                const QJsonObject client{{"server", serverUrl}, {"device_id", id},
                                         {"token", device.value("token").toString()},
                                         {"allow_insecure_lan", allowInsecureLan}}; // 每台设备保留独立令牌。
                // 写完并 commit 成功才认为配置生成成功，避免客户端拿到半份文件。
                return clientFile.write(QJsonDocument(client).toJson(QJsonDocument::Indented)) >= 0
                    && clientFile.commit(); // 原子写入，避免复制半个配置文件。
            };
            if (!writeClient(publicHost, QStringLiteral(""))
                || !writeClient(lanHost, QStringLiteral("-lan"))) { // 公网默认文件和局域网备用文件都必须生成。
                error << QStringLiteral("无法为设备 ") << id
                      << QStringLiteral(" 写入客户端配置。\n"); return 1; // 使用中文提示指出生成失败的具体设备。
            }
        }
        output << QStringLiteral("自动配置：") << configPath << Qt::endl; // 使用中文说明自动配置文件位置。
        output << QStringLiteral("公网请复制 client-device-*.json；局域网请复制 client-device-*-lan.json。") << Qt::endl; // 使用中文明确两种实际部署入口。
    }
    // 输出最终入口；0.0.0.0 表示绑定所有本机网卡，具体端口由 server.port() 返回。
    output << QStringLiteral("正在监听 ")
           << (secure ? QStringLiteral("wss://") : QStringLiteral("ws://")) << address.toString()
           << ':' << server.port() << Qt::endl;
    // 进入 Qt 事件循环；从这里开始持续处理 WebSocket 连接、消息和信号。
    return app.exec();
}
