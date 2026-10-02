#include "sheBeiZhuCe.h" // 实现注册记录的校验和原子保存。
#include "protocol.h" // 使用一致的设备 ID 格式限制。
#include <QDir> // 创建注册记录的父目录。
#include <QFile> // 读取重启前保存的身份。
#include <QFileInfo> // 解析存储文件的目录。
#include <QJsonArray> // 保存多台设备的凭据。
#include <QJsonDocument> // 解析和序列化 UTF-8 JSON。
#include <QJsonObject> // 表示一条设备记录。
#include <QSaveFile> // 完整保存后才替换注册文件。

namespace { // 本文件内部辅助函数不暴露给网络层。
constexpr qsizetype zuiDaDengJiShu = 4096; // 限制持久记录资源，与 32 个并发连接限制分开。
constexpr qint64 zuiDaWenJian = 4 * 1024 * 1024; // 最大读取 4 MiB，避免异常注册文件占用无限内存。

// 检查单个身份的格式，避免注册时写入今后无法登录的数据。
bool pingJuYouXiao(const DeviceCredential& pingJu)
{
    return protocol::validId(pingJu.id) && !pingJu.name.trimmed().isEmpty() // 设备 ID 合法且必须有显示名。
        && pingJu.name.size() <= 80 && pingJu.token.size() >= 32 && pingJu.token.size() <= 256; // 与现有认证参数限制一致。
}
} // 内部辅助函数结束。

// 查找已认可的身份；调用方不得把缺失身份当作已认证设备。
const DeviceCredential* SheBeiZhuCe::chaZhao(const QString& id) const
{
    for (const auto& pingJu : sheBei_) if (pingJu.id == id) return &pingJu; // 使用登记顺序保存与查找，最多 4096 条。
    return nullptr; // ID 不存在，需要走注册流程。
}

// 逐字节比较令牌；不能因开放登记而跳过已有身份的校验。
bool SheBeiZhuCe::yanZheng(const QString& id, const QString& token) const
{
    const auto* pingJu = chaZhao(id); // 先定位该身份认可的令牌。
    if (!pingJu) return false; // 未登记身份不能直接登录。
    const auto yuQi = pingJu->token.toUtf8(); // 用同一种编码比较令牌。
    const auto shiJi = token.toUtf8(); // 客户端字符串转换为字节序列。
    unsigned int chaYi = static_cast<unsigned int>(yuQi.size() ^ shiJi.size()); // 长度不同也要计入比较结果。
    for (qsizetype i = 0; i < yuQi.size(); ++i) { // 遍历全部预期字节，不在首个不同位置提前退出。
        chaYi |= static_cast<unsigned char>(yuQi[i]) // 累积预期字节与客户端对应字节的差异。
            ^ static_cast<unsigned char>(i < shiJi.size() ? shiJi[i] : 0); // 短令牌不足部分用零参与比较。
    }
    return chaYi == 0; // 仅完全一致才允许使用该身份。
}

// 合并预设设备和已保存的注册记录；加载失败时不改变当前注册表。
bool SheBeiZhuCe::duQu(const QList<DeviceCredential>& yuShe, const QString& luJing, QString* cuoWu)
{
    SheBeiZhuCe jieGuo; // 在局部对象完成校验，成功后才替换现有状态。
    if (yuShe.size() > zuiDaDengJiShu) { // 预设名单也不能超过永久身份总上限。
        *cuoWu = QStringLiteral("预设设备数量超过 4096 台。"); // 防止绕过注册表资源限制。
        return false; // 保留原注册表状态。
    }
    for (const auto& pingJu : yuShe) { // 检查 server.json 中保留的固定设备。
        if (!pingJuYouXiao(pingJu) || jieGuo.chaZhao(pingJu.id)) { // 固定 ID 不允许重复。
            *cuoWu = QStringLiteral("预设设备凭据无效或设备 ID 重复。"); // 明确报告配置问题。
            return false; // 拒绝部分加载。
        }
        for (const auto& yiYou : jieGuo.sheBei_) { // 固定和动态设备均不允许共用令牌。
            if (yiYou.token == pingJu.token) { // 重复令牌会让设备身份失去独立性。
                *cuoWu = QStringLiteral("设备访问令牌重复。"); // 错误信息不包含令牌内容。
                return false; // 不认可重复凭据。
            }
        }
        jieGuo.sheBei_.append(pingJu); // 暂存已校验的固定身份。
    }
    jieGuo.luJing_ = luJing; // 有永久存储路径才允许随后开放注册。
    if (!luJing.isEmpty() && QFile::exists(luJing)) { // 服务端重启时读取之前的注册记录。
        QFile wenJian(luJing); // 仅以只读方式打开身份库。
        if (!wenJian.open(QIODevice::ReadOnly) || wenJian.size() > zuiDaWenJian) { // 拒绝不可读或异常大的文件。
            *cuoWu = QStringLiteral("无法读取设备注册记录，或文件超过 4 MiB。"); // 不误认为没有注册过的设备。
            return false; // 不用空数据库覆盖原记录。
        }
        QJsonParseError jieXiCuoWu; // 保存 JSON 解析结果。
        const auto wenDang = QJsonDocument::fromJson(wenJian.readAll(), &jieXiCuoWu); // 解析已保存的设备数组。
        if (jieXiCuoWu.error != QJsonParseError::NoError || !wenDang.isArray()) { // 注册库根节点必须是数组。
            *cuoWu = QStringLiteral("设备注册记录不是有效的 JSON 数组。"); // 提示用户检查原文件。
            return false; // 不自动清空损坏记录。
        }
        for (const auto& jiLu : wenDang.array()) { // 按原登记顺序恢复每个设备。
            const auto duiXiang = jiLu.toObject(); // 读取这条身份的三个字段。
            const DeviceCredential pingJu{duiXiang.value("id").toString(), // 恢复设备 ID。
                duiXiang.value("name").toString(), duiXiang.value("token").toString()}; // 恢复显示名与认证凭据。
            if (!pingJuYouXiao(pingJu) || jieGuo.chaZhao(pingJu.id) || jieGuo.sheBei_.size() >= zuiDaDengJiShu) { // 防止无效、冲突或超限记录。
                *cuoWu = QStringLiteral("设备注册记录无效、ID 冲突或超过 4096 台。"); // 不向日志泄露凭据。
                return false; // 注册表必须整体有效。
            }
            for (const auto& yiYou : jieGuo.sheBei_) { // 检查动态身份与既有令牌的独立性。
                if (yiYou.token == pingJu.token) { // 不允许两台设备保存同一密码。
                    *cuoWu = QStringLiteral("设备注册记录中的访问令牌重复。"); // 只报告原因。
                    return false; // 保留原注册表。
                }
            }
            jieGuo.sheBei_.append(pingJu); // 登录表包含这台设备。
            jieGuo.dongTai_.append(pingJu); // 持久表仅包含动态身份。
        }
    }
    *this = jieGuo; // 完整校验成功后才替换运行状态。
    return true; // 可以启动监听并按配置决定是否开放登记。
}

// 原子写入动态设备记录；调用方只有在此函数成功后才更新内存身份。
bool SheBeiZhuCe::baoCun(const QList<DeviceCredential>& dongTai) const
{
    if (!QDir().mkpath(QFileInfo(luJing_).absolutePath())) return false; // 首次登记需要可写的记录目录。
    QJsonArray neiRong; // 序列化所有动态身份，以便重启时恢复。
    for (const auto& pingJu : dongTai) { // 每条记录只保存必要的身份字段。
        neiRong.append(QJsonObject{{"id", pingJu.id}, {"name", pingJu.name}, {"token", pingJu.token}}); // 不混入聊天内容或预设配置。
    }
    QSaveFile wenJian(luJing_); // 避免直接截断已有设备库。
    const auto ziJie = QJsonDocument(neiRong).toJson(QJsonDocument::Indented); // 使用 UTF-8 JSON 保存中文设备名。
    return ziJie.size() <= zuiDaWenJian && wenJian.open(QIODevice::WriteOnly) // 先确保持久文件不会超过读取上限。
        && wenJian.write(ziJie) == ziJie.size() && wenJian.commit(); // 完整提交后才认可保存成功。
}

// 开放登记新身份，并允许同一凭据的注册重试；从不覆盖已有身份。
QString SheBeiZhuCe::dengJi(const DeviceCredential& pingJu)
{
    if (!kaiFang()) return QStringLiteral("registration_disabled"); // 固定名单模式不接受新设备。
    if (!pingJuYouXiao(pingJu)) return QStringLiteral("invalid_credentials"); // 拒绝以后无法登录的格式。
    if (chaZhao(pingJu.id)) { // 注册请求可能因网络中断而重复。
        return yanZheng(pingJu.id, pingJu.token) ? QString{} : QStringLiteral("auth_failed"); // 相同凭据成功，不同令牌拒绝覆盖。
    }
    if (sheBei_.size() >= zuiDaDengJiShu) return QStringLiteral("registration_full"); // 永久身份数量与并发连接分别限制。
    for (const auto& yiYou : sheBei_) { // 新 ID 也不能复用其他设备的令牌。
        if (yiYou.token == pingJu.token) return QStringLiteral("invalid_credentials"); // 保持身份独立。
    }
    auto xinJiLu = dongTai_; // 先在临时列表加入设备，写失败可保持原状态。
    xinJiLu.append(pingJu); // 新身份只出现一次。
    if (!baoCun(xinJiLu)) return QStringLiteral("registration_write_failed"); // 磁盘失败时不能返回注册成功。
    dongTai_ = xinJiLu; // 永久保存成功后更新动态记录。
    sheBei_.append(pingJu); // 此时才允许网络层使用这台设备登录。
    return {}; // 成功无需暴露令牌。
}
