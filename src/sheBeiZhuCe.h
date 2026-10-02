#pragma once // 避免设备凭据和注册类被重复声明。
#include <QList> // 保存按登记顺序排列的设备，便于生成成员列表。
#include <QString> // 表示设备身份、显示名、令牌和存储路径。

// 表示服务端认可的一组设备凭据；兼容原有固定设备配置。
struct DeviceCredential { // 从网络头文件移到注册模块，让文件存储与网络通信分工明确。
    QString id; // 唯一设备 ID，用于绑定消息来源。
    QString name; // 成员列表显示的设备名称。
    QString token; // 登录凭据，不输出到日志或成员列表。
};

// 管理设备登记和永久记录；只有磁盘保存成功的新身份才能用于登录。
class SheBeiZhuCe final { // “设备注册”只负责身份数据，不直接操作 WebSocket。
public:
    // 加载固定身份和动态注册记录；冲突或文件损坏时返回中文错误。
    bool duQu(const QList<DeviceCredential>& yuShe, const QString& luJing, QString* cuoWu); // 空路径表示不启用开放注册。
    // 校验并登记身份；空字符串表示成功，其余结果为网络层可返回的协议错误码。
    QString dengJi(const DeviceCredential& pingJu); // 同一身份的重复登记不会重复写文件。
    // 检查已有 ID 与令牌是否一致，避免开放注册覆盖别人的身份。
    bool yanZheng(const QString& id, const QString& token) const; // 使用逐字节比较处理令牌。
    // 查找成员的显示信息；指针仅供调用方在下一次登记前短暂使用。
    const DeviceCredential* chaZhao(const QString& id) const; // 返回空指针表示未知身份。
    // 获取当前所有已认可的身份，网络层再选择需要展示的在线成员。
    const QList<DeviceCredential>& sheBei() const { return sheBei_; } // 返回只读引用，避免复制全部记录。
    // 有配置的存储路径才允许开放登记，旧测试和手动固定名单默认保持原认证行为。
    bool kaiFang() const { return !luJing_.isEmpty(); } // 防止只有内存记录却向客户端声称永久登记成功。
private:
    // 将新增记录列表原子保存；不保存预设身份，避免重复或覆盖 server.json。
    bool baoCun(const QList<DeviceCredential>& dongTai) const; // 写失败时调用方保持原内存记录。
    QList<DeviceCredential> sheBei_; // 包括预设与动态身份，供登录和成员查询使用。
    QList<DeviceCredential> dongTai_; // 单独保存自动登记的身份。
    QString luJing_; // registered-devices.json 的绝对路径。
};
