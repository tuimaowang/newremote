#pragma once
#include "message_client.h"
#include <QDateTime>
#include <QMainWindow>
#include <QMap>

class QLabel;
class QListWidget;
class QPushButton;
class QTextBrowser;
class QTextEdit;
class QStackedWidget;
class QResizeEvent;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(ClientProfile profile, QWidget* parent = nullptr);
    void connectToServer();
    MessageClient* client() { return &client_; }
protected:
    void resizeEvent(QResizeEvent* event) override;
private:
    struct Entry {
        QString id;
        QString text;
        QString status;
        QDateTime time;
        bool outgoing = false;
    };
    void editConnection();
    void updateDevices(const QJsonArray& devices);
    void selectDevice();
    void renderMessages();
    void updateSendState();
    void sendMessage();
    void append(const QString& peer, Entry entry);
    ClientProfile profile_;
    MessageClient client_;
    QMap<QString, QList<Entry>> history_;
    QMap<QString, QString> names_;
    QMap<QString, bool> online_;
    QMap<QString, QString> drafts_;
    QString selected_;
    QLabel* state_ = nullptr;
    QLabel* server_ = nullptr;
    QLabel* self_ = nullptr;
    QLabel* count_ = nullptr;
    QLabel* peerTitle_ = nullptr;
    QLabel* peerDetail_ = nullptr;
    QLabel* error_ = nullptr;
    QLabel* counter_ = nullptr;
    QListWidget* list_ = nullptr;
    QTextBrowser* messages_ = nullptr;
    QStackedWidget* conversation_ = nullptr;
    QTextEdit* input_ = nullptr;
    QPushButton* connect_ = nullptr;
    QPushButton* send_ = nullptr;
};
