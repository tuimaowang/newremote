#include "keHuDuanPeiZhi.h" // 实现本机身份与连接入口的保存逻辑。
#include "protocol.h" // 使用已有 UUID 工具生成符合协议规则的设备 ID。
#include <QDir> // 创建当前用户的配置目录。
#include <QFile> // 读取已有身份文件。
#include <QFileInfo> // 从配置路径提取父目录。
#include <QJsonDocument> // 在 JSON 文件和 Qt 对象之间转换。
#include <QJsonObject> // 保存服务器地址、身份和自动注册选项。
#include <QRandomGenerator> // 从系统随机源生成独立访问令牌。
#include <QSaveFile> // 临时文件写入成功后才替换配置，避免断电留下半份 JSON。
#include <QStandardPaths> // 使用用户可写目录，而不是要求安装目录可写。
#include <QSysInfo> // 用电脑名称作为新设备的默认显示名称。

// 返回身份文件路径；名称与旧的共享配置不同，因此不会误用 device-a 的旧令牌。
QString KeHuDuanPeiZhi::moRenLuJing()
{
    const auto muLu = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation); // 跟随当前登录用户和应用名称。
    return muLu.isEmpty() ? QString{} : QDir(muLu).filePath(QStringLiteral("client-identity.json")); // 无有效目录时让调用方报告错误。
}

// 写入本机配置；未提供保存路径的手动连接或内存测试不产生磁盘文件。
bool KeHuDuanPeiZhi::baoCun(const ClientProfile& peiZhi, QString* cuoWu)
{
    if (peiZhi.peiZhiLuJing.isEmpty()) return true; // 手动指定配置时保留原有只读语义。
    if (!QDir().mkpath(QFileInfo(peiZhi.peiZhiLuJing).absolutePath())) { // 确保首次运行也能创建用户配置目录。
        *cuoWu = QStringLiteral("无法创建客户端身份目录。"); // 明确区分目录权限和服务器认证问题。
        return false; // 不能保存身份时不继续声称注册准备成功。
    }
    QJsonArray beiYong; // JSON 数组保存按尝试顺序排列的备用入口。
    for (const auto& diZhi : peiZhi.beiYongDiZhi) beiYong.append(diZhi.toString()); // 保留地址顺序，供下次启动继续切换。
    const QJsonObject neiRong{ // 键名是机器可读契约，保持英文不翻译。
        {"server", peiZhi.server.toString()}, {"device_id", peiZhi.deviceId}, // 保存最近使用的入口及本机身份。
        {"token", peiZhi.token}, {"name", peiZhi.sheBeiMing}, // 保存令牌和设备显示名，令牌不写入日志。
        {"allow_insecure_lan", peiZhi.allowInsecureLan}, {"auto_register", peiZhi.ziDongZhuCe}, // 保留当前部署和接入模式。
        {"fallback_servers", beiYong}}; // 保存备用连接入口。
    QSaveFile wenJian(peiZhi.peiZhiLuJing); // QSaveFile 在 commit 成功前不会替换旧身份。
    const auto ziJie = QJsonDocument(neiRong).toJson(QJsonDocument::Indented); // 缩进便于用户检查配置。
    if (!wenJian.open(QIODevice::WriteOnly) || wenJian.write(ziJie) != ziJie.size() || !wenJian.commit()) { // 完整写入并提交才算保存成功。
        *cuoWu = QStringLiteral("无法保存客户端身份，原配置未被替换。"); // 避免将文件问题误报为令牌错误。
        return false; // 调用方必须处理失败，不能仅保存到内存。
    }
    return true; // 身份已经安全写入，可开始或重试注册。
}

// 为一个客户端生成独立身份；先保存再返回，避免断线重试产生重复注册。
bool KeHuDuanPeiZhi::chuangJianShenFen(ClientProfile* peiZhi, QString* cuoWu)
{
    auto xinPeiZhi = *peiZhi; // 保留服务器地址和保存路径，写失败时不破坏原参数。
    xinPeiZhi.deviceId = QStringLiteral("device-%1").arg(protocol::id()); // 每台新客户端都有独立 UUID。
    QByteArray suiJi(32, Qt::Uninitialized); // 准备 256 位随机原始令牌。
    for (auto& ziJie : suiJi) ziJie = static_cast<char>(QRandomGenerator::system()->generate() & 0xff); // 系统随机源避免可预测密码。
    xinPeiZhi.token = QString::fromLatin1(suiJi.toBase64()); // Base64 便于 JSON 保存且满足令牌长度限制。
    xinPeiZhi.sheBeiMing = QSysInfo::machineHostName().trimmed().left(80); // 默认使用电脑名称，限制长度以适配协议。
    if (xinPeiZhi.sheBeiMing.isEmpty()) xinPeiZhi.sheBeiMing = QStringLiteral("新设备"); // 没有电脑名时提供中文显示名。
    if (!baoCun(xinPeiZhi, cuoWu)) return false; // 保存失败则不使用临时身份发起注册。
    *peiZhi = xinPeiZhi; // 写入成功后才替换调用方的运行参数。
    return true; // 下次启动将读取同一身份。
}

// 读取身份或明确指定的连接文件；配置损坏时报告错误，不悄悄覆盖用户文件。
bool KeHuDuanPeiZhi::duQuZhiDing(const QString& luJing, ClientProfile* peiZhi, QString* cuoWu)
{
    QFile wenJian(luJing); // QFile 负责只读访问已有 JSON。
    if (!wenJian.open(QIODevice::ReadOnly) || wenJian.size() > 16384) { // 限制文件大小，与既有客户端配置一致。
        *cuoWu = QStringLiteral("无法读取客户端配置文件（最大 16 KiB）。"); // 说明读文件失败或文件超限。
        return false; // 不用空身份继续连接。
    }
    QJsonParseError jieXiCuoWu; // 保存 JSON 语法错误的结果。
    const auto wenDang = QJsonDocument::fromJson(wenJian.readAll(), &jieXiCuoWu); // 解析 UTF-8 JSON 数据。
    if (jieXiCuoWu.error != QJsonParseError::NoError || !wenDang.isObject()) { // 配置必须以 JSON 对象为根节点。
        *cuoWu = QStringLiteral("客户端配置不是有效的 JSON 对象。"); // 使用中文解释损坏的配置。
        return false; // 不重建或覆盖损坏文件。
    }
    const auto neiRong = wenDang.object(); // 后续按键读取连接参数。
    ClientProfile jieGuo; // 在局部对象完成校验后再交给调用方。
    jieGuo.server = QUrl(neiRong.value("server").toString()); // 读取当前连接入口。
    jieGuo.deviceId = neiRong.value("device_id").toString(); // 读取设备身份。
    jieGuo.token = neiRong.value("token").toString(); // 读取令牌，仅用于认证。
    jieGuo.allowInsecureLan = neiRong.value("allow_insecure_lan").toBool(); // 不擅自放宽手动连接的传输要求。
    jieGuo.sheBeiMing = neiRong.value("name").toString(); // 新配置带有显示名，旧配置可为空。
    *cuoWu = MessageClient::validate(jieGuo); // 复用网络层对地址、ID 和令牌的检查。
    if (!cuoWu->isEmpty()) return false; // 无效配置不能启动连接。
    *peiZhi = jieGuo; // 手动配置默认关闭自动注册且不保存。
    return true; // 调用方可以按明确指定的身份连接。
}

// 自动模式使用本机独立配置；不存在时生成身份和两个部署入口。
bool KeHuDuanPeiZhi::duQuZiDong(const QString& luJing, ClientProfile* peiZhi, QString* cuoWu)
{
    if (luJing.isEmpty()) { // 系统未提供用户目录时无法永久保存身份。
        *cuoWu = QStringLiteral("系统没有提供可写的客户端身份目录。"); // 不要求用户启动本机服务端。
        return false; // 防止每次启动都生成一个临时身份。
    }
    if (!QFile::exists(luJing)) { // 新电脑不需要已有 JSON，也不需要运行服务端生成文件。
        ClientProfile xinPeiZhi; // 首次运行组装自动连接参数。
        xinPeiZhi.server = QUrl(QStringLiteral("ws://192.168.3.63:62843")); // 优先尝试当前部署的局域网入口。
        xinPeiZhi.beiYongDiZhi = {QUrl(QStringLiteral("ws://112.26.74.220:62843"))}; // 局域网不可达时转公网入口。
        xinPeiZhi.allowInsecureLan = true; // 延续当前部署的 WS 模式，不改变服务端 TLS 配置。
        xinPeiZhi.ziDongZhuCe = true; // 用户已选择开放加入，新设备自动登记。
        xinPeiZhi.peiZhiLuJing = luJing; // 身份只写在本机用户目录。
        if (!chuangJianShenFen(&xinPeiZhi, cuoWu)) return false; // 注册前先永久保存身份。
        *peiZhi = xinPeiZhi; // 将已保存的参数交给客户端和窗口。
        return true; // 允许立即自动连接。
    }
    ClientProfile jieGuo; // 暂存已保存身份，避免读取失败影响调用方。
    if (!duQuZhiDing(luJing, &jieGuo, cuoWu)) return false; // 首先检查基础地址和身份。
    QFile wenJian(luJing); // 读取自动模式额外保存的入口和注册选项。
    if (!wenJian.open(QIODevice::ReadOnly) || wenJian.size() > 16384) { // 再次读取也检查文件状态和大小。
        *cuoWu = QStringLiteral("无法读取本机自动连接配置。"); // 文件变化或权限问题明确报告。
        return false; // 不猜测配置内容。
    }
    QJsonParseError jieXiCuoWu; // 检查两次读取之间文件是否被改坏。
    const auto wenDang = QJsonDocument::fromJson(wenJian.readAll(), &jieXiCuoWu); // 解析额外自动参数。
    if (jieXiCuoWu.error != QJsonParseError::NoError || !wenDang.isObject()) { // 拒绝损坏或非对象根节点。
        *cuoWu = QStringLiteral("本机自动连接配置不是有效的 JSON 对象。"); // 保留文件供用户检查。
        return false; // 不自行覆盖旧文件。
    }
    const auto neiRong = wenDang.object(); // 读取保存的自动模式字段。
    jieGuo.ziDongZhuCe = neiRong.value("auto_register").toBool(true); // 兼容自动配置缺少该字段的情况。
    for (const auto& diZhi : neiRong.value("fallback_servers").toArray()) { // 恢复地址尝试顺序。
        const QUrl beiYong(diZhi.toString()); // 备用地址也必须经过 URL 校验。
        if (beiYong != jieGuo.server && !jieGuo.beiYongDiZhi.contains(beiYong)) jieGuo.beiYongDiZhi.append(beiYong); // 去除主地址和重复项。
    }
    jieGuo.peiZhiLuJing = luJing; // 以后保存最近成功地址时仍写回同一个身份文件。
    *cuoWu = MessageClient::validate(jieGuo); // 校验自动模式的所有备用入口及显示名。
    if (!cuoWu->isEmpty()) return false; // 无效备用地址不加入重连队列。
    *peiZhi = jieGuo; // 使用已保存的身份，不重新生成令牌。
    return true; // 启动和重启共用同一身份。
}
