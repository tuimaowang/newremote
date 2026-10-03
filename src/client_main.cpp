#include "main_window.h"
#include "keHuDuanPeiZhi.h" // 将身份生成和配置读写交给独立模块，入口只负责组装程序。
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QTimer>

// 启动客户端；只准备本机独立身份并自动连接服务器，不再提供固定配置入口。
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("FSRemoteMessages");
    app.setOrganizationName("FSRemote");
    app.setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);
    ClientProfile profile; // 窗口和网络层共用经过校验的连接参数。
    QString cuoWu; // 配置模块返回中文文件或校验错误。
    const bool duQuChengGong = KeHuDuanPeiZhi::duQuZiDong(KeHuDuanPeiZhi::moRenLuJing(), &profile, &cuoWu); // 所有客户端统一生成或恢复本机身份。
    if (!duQuChengGong) { // 读写失败时给出准确提示。
        QMessageBox::critical(nullptr, QStringLiteral("配置错误"), cuoWu); // 中文提示不泄露令牌。
        return 1; // 不用不完整身份打开连接。
    }
    MainWindow window(profile); // 创建窗口并传入本机独立身份。
    window.show(); // 先显示窗口，用户能观察连接与注册进度。
    QTimer::singleShot(0, &window, &MainWindow::connectToServer); // 进入 Qt 事件循环后自动发起连接。
    return app.exec(); // Qt 事件循环驱动网络、计时器和界面事件。
}
