#pragma once
#include "sheBeiZhuCe.h" // 设备凭据和永久注册记录由独立模块管理。
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QQueue> // 公共聊天室使用有界队列，避免接收回调直接广播。
#include <QObject>
#include <QSet>
#include <QSslConfiguration>
#include <QTimer>
#include <QWebSocketServer>
#include <memory>

class QWebSocket;

class MessageServer final : public QObject {
    Q_OBJECT
public:
    explicit MessageServer(QList<DeviceCredential> credentials, QObject* parent = nullptr);
    ~MessageServer() override;
    bool listen(const QHostAddress& address, quint16 port,
                const QSslConfiguration* tls = nullptr,
                bool allowInsecureLan = false);
    quint16 port() const;
    QString errorString() const;
    void stop();
    // 在监听前加载永久身份库；空路径保持只允许预设设备的旧模式。
    bool qiYongZhuCe(const QString& luJing); // 服务端入口明确决定是否开放自动注册。
signals:
    void activity(const QString& text);
private:
    struct Peer {
        QString deviceId;
        qint64 connectedAt = 0;
        qint64 lastPong = 0;
        qint64 rateStart = 0;
        int rateCount = 0;
        QSet<QString> requests;
    };
    struct Pending {
        QWebSocket* sender = nullptr;
        QWebSocket* target = nullptr;
        QString requestId;
        qint64 expiresAt = 0;
    };
    void accept();
    void receive(QWebSocket* socket, const QString& text);
    void disconnected(QWebSocket* socket);
    bool send(QWebSocket* socket, const QJsonObject& message);
    void reply(QWebSocket* socket, const QString& type, const QString& requestId,
               QJsonObject payload = {});
    void fail(QWebSocket* socket, const QString& requestId, const QString& code);
    QJsonArray roster() const;
    void broadcastRoster();
    void fenFaLiaoTian(); // 按入队顺序广播公共聊天室消息。
    void tick();
    QList<DeviceCredential> credentials_;
    SheBeiZhuCe zhuCeBiao_; // 同时查询原有固定身份和重启后恢复的新身份。
    std::unique_ptr<QWebSocketServer> server_;
    QHash<QWebSocket*, Peer> peers_;
    QHash<QString, QWebSocket*> online_;
    QHash<QString, Pending> pending_;
    struct LiaoTianRenWu { QWebSocket* sender; QString requestId; QString text; }; // 保存认证连接与待广播内容。
    QQueue<LiaoTianRenWu> liaoTianDuilie_; // 有界内存队列，不保存离线消息。
    quint64 liaoTianXuhao_ = 0; // 服务器运行期间单调递增的房间序号。
    bool liaoTianYipaicheng_ = false; // 防止重复安排队列排空任务。
    QElapsedTimer clock_;
    QTimer timer_;
    QString error_;
};
