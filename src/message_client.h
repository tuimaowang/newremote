#pragma once
#include <QElapsedTimer>
#include <QDateTime> // 房间事件携带服务器时间。
#include <QHash>
#include <QJsonArray>
#include <QList> // 保存内外网备用入口的尝试顺序。
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

struct ClientProfile {
    QUrl server = QUrl(QStringLiteral("ws://127.0.0.1:17890"));
    QString deviceId;
    QString token;
    // LAN demo mode is explicit because plain WebSocket is not encrypted.
    bool allowInsecureLan = false;
    bool ziDongZhuCe = false; // 自动模式允许登记新身份，手动固定凭据默认不自动登记。
    QString sheBeiMing; // 首次登记时向服务端提供电脑显示名。
    QList<QUrl> beiYongDiZhi; // 当前地址失败时按顺序尝试其余入口。
    QString peiZhiLuJing; // 本机身份保存路径；空路径表示不写磁盘的手动连接。
};

class MessageClient final : public QObject {
    Q_OBJECT
public:
    explicit MessageClient(QObject* parent = nullptr);
    ~MessageClient() override;
    static QString validate(const ClientProfile& profile);
    void start(const ClientProfile& profile);
    void stop();
    bool online() const { return online_; }
    bool active() const { return wanted_; }
    const ClientProfile& profile() const { return profile_; }
    QString sendMessage(const QString& target, const QString& text);
    QString faSongLiaoTian(const QString& text); // 发送公共聊天室文本并返回关联请求 ID。
signals:
    void stateChanged(const QString& text, bool online);
    void devicesChanged(const QJsonArray& devices);
    void incoming(const QString& id, const QString& from, const QString& text);
    void liaoTianDaoDa(const QString& requestId, const QString& from, const QString& text,
                      qint64 sequence, const QDateTime& at); // 通知界面显示服务器排序后的消息。
    void deliveryChanged(const QString& id, const QString& state);
    void problem(const QString& text);
    void peiZhiGengXin(const ClientProfile& peiZhi); // 身份或成功入口变化时通知界面同步展示。
private:
    void open();
    bool send(const QJsonObject& message);
    void receive(const QString& text);
    void losePending();
    // 合并错误和断线回调的重试安排；每次失败最多切换一次连接入口。
    void anPaiChongLian(); // 避免同一连接触发多次重连计时器。
    QWebSocket socket_;
    QTimer retry_;
    QTimer deadline_;
    QTimer maintenance_;
    QElapsedTimer clock_;
    QHash<QString, qint64> pending_;
    QHash<QString, qint64> liaoTianDaifa_; // 等待服务器广播确认的聊天室请求。
    qint64 zuiHouXuhao_ = 0; // 过滤同一连接内的重复或倒序房间事件。
    ClientProfile profile_;
    QString authRequest_;
    bool online_ = false;
    bool wanted_ = false;
    int retryDelay_ = 1000;
    qint64 lastPong_ = 0;
    bool yiGengHuanShenFen_ = false; // 每次用户发起连接最多自动替换一次冲突身份，避免无限注册。
};
