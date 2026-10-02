#pragma once // 防止同一份配置类声明被多个头文件重复包含。
#include "message_client.h" // 复用客户端连接参数，避免配置和网络层各自维护不同字段。

// 管理本机独立身份和连接入口；只处理文件，不创建窗口或建立网络连接。
class KeHuDuanPeiZhi final { // 使用拼音类名表达“客户端配置”的职责。
public:
    // 返回当前用户的身份文件路径；安装目录只分发程序，不分发设备身份。
    static QString moRenLuJing(); // 使用 Qt 的用户应用数据目录保存配置。
    // 读取本机配置；首次运行自动创建身份，失败时通过错误参数说明原因。
    static bool duQuZiDong(const QString& luJing, ClientProfile* peiZhi, QString* cuoWu); // 路径参数也便于测试隔离真实配置。
    // 读取用户通过 --config 明确指定的旧式配置，不自动注册或修改该文件。
    static bool duQuZhiDing(const QString& luJing, ClientProfile* peiZhi, QString* cuoWu); // 保留已有部署的固定身份连接方式。
    // 原子保存身份、备用地址和最近成功的地址；失败时不覆盖原文件。
    static bool baoCun(const ClientProfile& peiZhi, QString* cuoWu); // 保存位置由自动配置的路径字段决定。
    // 生成新的独立身份并先写入磁盘；只有保存成功才更新调用方的参数。
    static bool chuangJianShenFen(ClientProfile* peiZhi, QString* cuoWu); // 重试注册时仍使用已经保存的同一身份。
};
