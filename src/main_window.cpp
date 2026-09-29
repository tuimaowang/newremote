#include "main_window.h"
#include "protocol.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QTextBrowser>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

// 构建公共聊天室界面；设备列表只展示成员，发送无需选择目标。
MainWindow::MainWindow(ClientProfile profile, QWidget* parent)
    : QMainWindow(parent), profile_(std::move(profile)), client_(this)
{
    setWindowTitle(QStringLiteral("FSRemote · 消息"));
    setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    setMinimumSize(760, 540);
    resize(1000, 700);
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget#root { background: #ffffff; }
        QWidget { color: #202832; font-family: 'Microsoft YaHei UI'; font-size: 13px; }
        QFrame#header { border-bottom: 1px solid #e3e7ec; }
        QFrame#sidebar { background: #f6f8fa; border-right: 1px solid #e3e7ec; }
        QFrame#peerHeader { border-bottom: 1px solid #e9edf1; }
        QFrame#composer { border-top: 1px solid #e3e7ec; }
        QLabel#brand { font-size: 19px; font-weight: 600; }
        QLabel#heading { font-size: 15px; font-weight: 600; }
        QLabel#muted { color: #77818e; font-size: 12px; }
        QLabel#error { color: #b23a38; background: #fff2f1; padding: 8px 12px; }
        QPushButton { background: #ffffff; border: 1px solid #d8dee6; border-radius: 4px; padding: 7px 16px; }
        QPushButton:hover { background: #f1f5fb; border-color: #a7bbdc; }
        QPushButton:disabled { color: #9ba3ae; background: #f4f5f7; border-color: #e3e7ec; }
        QPushButton#primary { background: #3a7bfc; color: white; border: 1px solid #3a7bfc; }
        QPushButton#primary:hover { background: #2768e4; }
        QPushButton#primary:disabled { background: #edf1f6; color: #9ba3ae; border-color: #e3e7ec; }
        QToolButton { border: none; border-radius: 4px; padding: 6px; }
        QToolButton:hover { background: #edf2f8; }
        QListWidget { background: transparent; border: none; outline: none; }
        QListWidget::item { margin: 2px 8px; padding: 10px 8px; border-radius: 4px; }
        QListWidget::item:selected { color: #2261cf; background: #e6efff; }
        QListWidget::item:hover:!selected { background: #edf0f4; }
        QTextBrowser { border: none; background: #ffffff; padding: 14px; }
        QTextEdit#input { border: none; background: #ffffff; padding: 4px; }
        QLineEdit { border: 1px solid #d8dee6; border-radius: 4px; padding: 7px; }
        QLineEdit:focus { border-color: #3a7bfc; }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
        QScrollBar::handle:vertical { background: #ccd3dc; border-radius: 4px; min-height: 24px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )"));

    auto* root = new QWidget;
    root->setObjectName("root");
    auto* outer = new QVBoxLayout(root);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* header = new QFrame;
    header->setObjectName("header");
    header->setFixedHeight(66);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(20, 10, 18, 10);
    auto* logo = new QLabel;
    logo->setPixmap(windowIcon().pixmap(30, 30));
    logo->setFixedSize(32, 32);
    headerLayout->addWidget(logo);
    auto* brand = new QLabel(QStringLiteral("FSRemote"));
    brand->setObjectName("brand");
    headerLayout->addWidget(brand);
    auto* subtitle = new QLabel(QStringLiteral("消息"));
    subtitle->setObjectName("muted");
    headerLayout->addWidget(subtitle);
    headerLayout->addStretch();
    state_ = new QLabel(QStringLiteral("未连接"));
    state_->setObjectName("connectionState");
    state_->setTextFormat(Qt::PlainText);
    headerLayout->addWidget(state_);
    connect_ = new QPushButton(QStringLiteral("连接"));
    connect_->setObjectName("connectButton");
    headerLayout->addWidget(connect_);
    auto* settings = new QToolButton;
    settings->setIcon(QIcon(QStringLiteral(":/settings.svg")));
    settings->setIconSize(QSize(20, 20));
    settings->setFixedSize(34, 34);
    settings->setToolTip(QStringLiteral("连接设置"));
    settings->setAccessibleName(QStringLiteral("连接设置"));
    headerLayout->addWidget(settings);
    outer->addWidget(header);

    auto* content = new QHBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    auto* sidebar = new QFrame;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(240);
    auto* left = new QVBoxLayout(sidebar);
    left->setContentsMargins(0, 0, 0, 0);
    auto* section = new QHBoxLayout;
    section->setContentsMargins(20, 20, 18, 10);
    auto* title = new QLabel(QStringLiteral("在线设备")); // 侧栏只展示成员，不决定聊天目标。
    title->setObjectName("heading");
    section->addWidget(title);
    section->addStretch();
    count_ = new QLabel(QStringLiteral("0 在线"));
    count_->setObjectName("muted");
    section->addWidget(count_);
    left->addLayout(section);
    list_ = new QListWidget;
    list_->setObjectName("deviceList");
    list_->setIconSize(QSize(22, 22));
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setTextElideMode(Qt::ElideRight);
    left->addWidget(list_, 1);
    self_ = new QLabel(profile_.deviceId.isEmpty() ? QStringLiteral("本机未配置") : profile_.deviceId);
    self_->setObjectName("muted");
    self_->setTextFormat(Qt::PlainText);
    self_->setWordWrap(true);
    self_->setMargin(18);
    left->addWidget(self_);
    content->addWidget(sidebar);

    auto* right = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(0);
    auto* peerHeader = new QFrame;
    peerHeader->setObjectName("peerHeader");
    peerHeader->setFixedHeight(78);
    auto* peerLayout = new QVBoxLayout(peerHeader);
    peerLayout->setContentsMargins(22, 14, 22, 14);
    peerTitle_ = new QLabel(QStringLiteral("公共聊天室")); // 全部客户端共用一个房间。
    peerTitle_->setObjectName("heading");
    peerTitle_->setTextFormat(Qt::PlainText);
    peerTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    peerDetail_ = new QLabel(QStringLiteral("连接后与所有在线设备交流")); // 显示当前房间范围。
    peerDetail_->setObjectName("muted");
    peerDetail_->setTextFormat(Qt::PlainText);
    peerDetail_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    peerLayout->addWidget(peerTitle_);
    peerLayout->addWidget(peerDetail_);
    right->addWidget(peerHeader);
    error_ = new QLabel;
    error_->setObjectName("error");
    error_->setTextFormat(Qt::PlainText);
    error_->setWordWrap(true);
    error_->hide();
    right->addWidget(error_);
    conversation_ = new QStackedWidget;
    auto* empty = new QLabel(QStringLiteral("暂无消息"));
    empty->setObjectName("muted");
    empty->setAlignment(Qt::AlignCenter);
    conversation_->addWidget(empty);
    messages_ = new QTextBrowser;
    messages_->setObjectName("messageHistory");
    messages_->setOpenLinks(false);
    messages_->setOpenExternalLinks(false);
    messages_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    conversation_->addWidget(messages_);
    right->addWidget(conversation_, 1);
    auto* composer = new QFrame;
    composer->setObjectName("composer");
    composer->setFixedHeight(150);
    auto* composeLayout = new QVBoxLayout(composer);
    composeLayout->setContentsMargins(18, 12, 18, 12);
    input_ = new QTextEdit;
    input_->setObjectName("input");
    input_->setAcceptRichText(false);
    input_->setPlaceholderText(QStringLiteral("消息"));
    composeLayout->addWidget(input_, 1);
    auto* sendRow = new QHBoxLayout;
    counter_ = new QLabel(QStringLiteral("0 / 4000"));
    counter_->setObjectName("muted");
    sendRow->addWidget(counter_);
    sendRow->addStretch();
    send_ = new QPushButton(style()->standardIcon(QStyle::SP_ArrowForward), QStringLiteral("发送"));
    send_->setObjectName("primary");
    send_->setFixedSize(96, 34);
    send_->setAccessibleName(QStringLiteral("发送消息"));
    sendRow->addWidget(send_);
    composeLayout->addLayout(sendRow);
    right->addWidget(composer);
    content->addLayout(right, 1);
    outer->addLayout(content, 1);
    server_ = new QLabel(profile_.server.toString());
    server_->setObjectName("muted");
    server_->setTextFormat(Qt::PlainText);
    server_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    server_->setToolTip(profile_.server.toString());
    server_->setContentsMargins(18, 6, 18, 6);
    server_->setFixedHeight(30);
    server_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(server_);
    setCentralWidget(root);

    connect(settings, &QToolButton::clicked, this, &MainWindow::editConnection);
    connect(connect_, &QPushButton::clicked, this, [this] {
        if (client_.active()) client_.stop();
        else if (!MessageClient::validate(profile_).isEmpty()) editConnection();
        else connectToServer();
    });
    // 成员列表不作为聊天目标；公共房间始终保持选中。
    connect(input_, &QTextEdit::textChanged, this, &MainWindow::updateSendState);
    connect(send_, &QPushButton::clicked, this, &MainWindow::sendMessage);
    connect(&client_, &MessageClient::stateChanged, this, [this](const QString& text, bool online) {
        state_->setText(text);
        state_->setStyleSheet(online ? "color: #21875b;" : "color: #77818e;");
        connect_->setText(client_.active() ? QStringLiteral("断开") : QStringLiteral("连接"));
        if (online) error_->hide();
        updateSendState();
    });
    connect(&client_, &MessageClient::problem, this, [this](const QString& text) {
        error_->setText(text);
        error_->show();
    });
    connect(&client_, &MessageClient::devicesChanged, this, &MainWindow::updateDevices);
    connect(&client_, &MessageClient::incoming, this,
        [this](const QString& id, const QString& from, const QString& text) {
            append(from, {id, text, QStringLiteral("已收到"), QDateTime::currentDateTime(), false});
        });
    connect(&client_, &MessageClient::liaoTianDaoDa, this,
        [this](const QString& id, const QString& from, const QString& text, qint64, const QDateTime& at) { // 展示服务器排序的房间广播。
            if (from == profile_.deviceId) return; // 自己发送的条目已经在本地等待确认。
            append(QStringLiteral("gongGongLiaoTian"), {id, text, names_.value(from, from), at.toLocalTime(), false}); // 保留来源和时间。
        });
    connect(&client_, &MessageClient::deliveryChanged, this,
        [this](const QString& id, const QString& status) {
            for (auto it = history_.begin(); it != history_.end(); ++it) {
                for (auto& entry : it.value()) {
                    if (entry.id == id) { entry.status = status; renderMessages(); return; }
                }
            }
        });
    updateSendState();
    selected_ = QStringLiteral("gongGongLiaoTian"); // 初始化唯一房间的本地历史键。
}

void MainWindow::connectToServer()
{
    error_->hide();
    client_.start(profile_);
}

// 编辑服务器和设备凭据，切换身份时清除原房间历史。
void MainWindow::editConnection()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("连接设置"));
    dialog.setMinimumWidth(460);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* address = new QLineEdit(profile_.server.toString());
    auto* deviceId = new QLineEdit(profile_.deviceId);
    auto* token = new QLineEdit(profile_.token);
    address->setMaxLength(512);
    deviceId->setMaxLength(80);
    token->setMaxLength(256);
    token->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("服务器"), address);
    form->addRow(QStringLiteral("设备 ID"), deviceId);
    form->addRow(QStringLiteral("访问令牌"), token);
    layout->addLayout(form);
    auto* validation = new QLabel;
    validation->setWordWrap(true);
    validation->setStyleSheet("color: #b23a38;");
    layout->addWidget(validation);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("连接"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        ClientProfile next{QUrl(address->text().trimmed()), deviceId->text().trimmed(), token->text(),
                           profile_.allowInsecureLan};
        const auto error = MessageClient::validate(next);
        if (!error.isEmpty()) { validation->setText(error); return; }
        const bool differentIdentity = next.deviceId != profile_.deviceId || next.server != profile_.server;
        client_.stop();
        if (differentIdentity) { // 切换账号或服务器时丢弃旧房间历史。
            history_.clear(); selected_.clear(); names_.clear(); drafts_.clear(); input_->clear();
            selected_ = QStringLiteral("gongGongLiaoTian"); // 新连接仍展示公共聊天室。
        }
        profile_ = next;
        self_->setText(profile_.deviceId);
        server_->setText(profile_.server.toString());
        server_->setToolTip(profile_.server.toString());
        renderMessages();
        dialog.accept();
        connectToServer();
    });
    dialog.exec();
}

// 更新房间成员列表及在线人数，包括当前设备。
void MainWindow::updateDevices(const QJsonArray& devices)
{
    online_.clear();
    QSignalBlocker blocker(list_);
    list_->clear();
    int connected = 0;
    for (const auto& value : devices) {
        const auto device = value.toObject();
        const auto id = device.value("id").toString();
        const auto name = device.value("name").toString();
        const bool online = device.value("online").toBool();
        names_[id] = name;
        online_[id] = online;
        if (id == profile_.deviceId) { // 本机也计入公共房间在线人数。
            self_->setText(QStringLiteral("本机  %1\n%2").arg(name, id));
            if (online) ++connected; // 成员总数包括自己。
            continue;
        }
        if (online) ++connected;
        auto* item = new QListWidgetItem(style()->standardIcon(QStyle::SP_ComputerIcon),
            QStringLiteral("%1\n%2").arg(name, online ? QStringLiteral("在线") : QStringLiteral("离线")), list_);
        item->setData(Qt::UserRole, id);
        item->setToolTip(QStringLiteral("%1\n%2").arg(name, id));
        if (!online) item->setForeground(QColor("#8b95a3"));
        // 成员不对应单独对话。
    }
    count_->setText(QStringLiteral("%1 在线").arg(connected));
    // 不自动选择设备，避免误示为私聊。
    blocker.unblock();
    selectDevice();
}

// 保持唯一公共房间为当前会话，成员选择不改变发送目标。
void MainWindow::selectDevice()
{
    peerTitle_->setText(QStringLiteral("公共聊天室")); // 列表选择不改变房间。
    peerDetail_->setText(QStringLiteral("%1 台设备在线").arg(count_->text().section(' ', 0, 0))); // 显示房间成员数。
    renderMessages();
    updateSendState();
}

void MainWindow::append(const QString& peer, Entry entry)
{
    auto& entries = history_[peer];
    entries.append(std::move(entry));
    if (entries.size() > 200) entries.removeFirst();
    renderMessages();
}

// 将公共房间纯文本安全转义后显示，发送方单独显示广播状态。
void MainWindow::renderMessages()
{
    const auto entries = history_.value(selected_);
    conversation_->setCurrentIndex(entries.isEmpty() ? 0 : 1);
    const bool atBottom = messages_->verticalScrollBar()->value()
        >= messages_->verticalScrollBar()->maximum() - 4;
    const int scroll = messages_->verticalScrollBar()->value();
    QString html = QStringLiteral("<html><body style='font-size:13px;color:#202832;'>");
    for (const auto& entry : entries) {
        const auto who = entry.outgoing ? QStringLiteral("我") : entry.status; // 收到的消息用发送设备名称署名。
        html += QStringLiteral("<p style='margin-top:16px;margin-bottom:5px;color:%1;'>%2"
            "&nbsp;&nbsp;<span style='font-size:11px;color:#8993a0;'>%3</span></p>")
            .arg(entry.outgoing ? "#3a7bfc" : "#21875b", who.toHtmlEscaped(), entry.time.toString("HH:mm:ss"));
        auto body = entry.text.toHtmlEscaped();
        body.replace('\n', QStringLiteral("<br>"));
        html += QStringLiteral("<p style='margin-top:0;margin-bottom:4px;'>%1</p>").arg(body);
        if (entry.outgoing) html += QStringLiteral("<p style='font-size:11px;color:#8993a0;margin-top:0;'>%1</p>")
            .arg(entry.status.toHtmlEscaped()); // 仅发送消息需要展示发送状态。
    }
    html += QStringLiteral("</body></html>");
    messages_->setHtml(html);
    messages_->verticalScrollBar()->setValue(atBottom ? messages_->verticalScrollBar()->maximum() : scroll);
}

// 只在本机在线且正文合法时允许向公共房间发送。
void MainWindow::updateSendState()
{
    const auto text = input_->toPlainText();
    counter_->setText(QStringLiteral("%1 / 4000").arg(text.size()));
    counter_->setStyleSheet(text.size() > protocol::maxTextLength ? "color: #b23a38;" : "color: #77818e;");
    input_->setEnabled(client_.online()); // 公共房间不依赖成员选择。
    send_->setEnabled(client_.online() && !text.trimmed().isEmpty()
        && text.size() <= protocol::maxTextLength);
}

// 提交房间消息并显示等待服务器确认的本地条目。
void MainWindow::sendMessage()
{
    const auto text = input_->toPlainText();
    const auto id = client_.faSongLiaoTian(text); // 广播给所有当前在线设备。
    if (id.isEmpty()) {
        error_->setText(QStringLiteral("消息未发送，请检查连接或稍后重试"));
        error_->show();
        return;
    }
    append(selected_, {id, text, QStringLiteral("等待服务器"), QDateTime::currentDateTime(), true}); // 先显示待确认状态。
    input_->clear();
    input_->setFocus();
}

// 适配窗口尺寸，保持服务器地址不会挤占界面宽度。
void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (!server_ || !peerTitle_) return;
    server_->setText(server_->fontMetrics().elidedText(profile_.server.toString(), Qt::ElideMiddle,
                                                     qMax(200, width() - 40)));
    // 房间标题长度固定，不需要对设备名作尺寸裁剪。
}
