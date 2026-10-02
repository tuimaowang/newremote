#include "main_window.h"
#include "keHuDuanPeiZhi.h" // 将身份生成和配置读写交给独立模块，入口只负责组装程序。
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QTimer>

// 启动客户端；默认自动准备本机身份并连接，--config 保留手动固定配置模式。
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("FSRemoteMessages");
    app.setOrganizationName("FSRemote");
    app.setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"config", QStringLiteral("使用指定客户端 JSON 配置，以固定身份自动连接。"), QStringLiteral("路径")}); // 帮助使用中文，选项名保持兼容。
    parser.process(app);
    ClientProfile profile; // 窗口和网络层共用经过校验的连接参数。
    QString cuoWu; // 配置模块返回中文文件或校验错误。
    const bool zhiDing = parser.isSet("config"); // 用户明确指定配置时不自动登记新身份。
    const bool duQuChengGong = zhiDing // 按用户选择决定加载模式。
        ? KeHuDuanPeiZhi::duQuZhiDing(parser.value("config"), &profile, &cuoWu) // 手动模式读取原有 JSON，不修改文件。
        : KeHuDuanPeiZhi::duQuZiDong(KeHuDuanPeiZhi::moRenLuJing(), &profile, &cuoWu); // 新电脑自动生成并保存身份，不再等待本机服务端配置。
    if (!duQuChengGong) { // 读写失败时给出准确提示。
        QMessageBox::critical(nullptr, QStringLiteral("配置错误"), cuoWu); // 中文提示不泄露令牌。
        return 1; // 不用不完整身份打开连接。
    }
    MainWindow window(profile); // 创建窗口并传入本机独立身份。
    window.show(); // 先显示窗口，用户能观察连接与注册进度。
    QTimer::singleShot(0, &window, &MainWindow::connectToServer); // 进入 Qt 事件循环后自动发起连接。
    return app.exec(); // Qt 事件循环驱动网络、计时器和界面事件。
}
