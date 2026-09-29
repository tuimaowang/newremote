#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

namespace {
// Only synthetic, fixed-format probe messages are accepted by this temporary demo.
const QString greeting = QStringLiteral("FSREMOTE-DEMO/1 READY");
void log(const QString& text)
{
    QTextStream(stdout) << QDateTime::currentDateTime().toString("HH:mm:ss")
                        << " " << text << Qt::endl;
}

void boundSocket(QWebSocket* socket)
{
    socket->setMaxAllowedIncomingFrameSize(256);
    socket->setMaxAllowedIncomingMessageSize(256);
    socket->setReadBufferSize(4096);
    QObject::connect(socket, &QWebSocket::binaryMessageReceived, socket,
                     [socket] { socket->abort(); });
    // Abort fatal frame errors before Qt resumes parsing the rejected frame.
    QObject::connect(socket, &QWebSocket::aboutToClose, socket,
        [socket, aborting = false]() mutable {
            if (aborting) return;
            if (socket->closeCode() == QWebSocketProtocol::CloseCodeTooMuchData
                || socket->closeCode() == QWebSocketProtocol::CloseCodeProtocolError
                || socket->closeCode() == QWebSocketProtocol::CloseCodeWrongDatatype) {
                aborting = true;
                socket->abort();
                aborting = false;
            }
        });
}

int serve(QCoreApplication& app, const QHostAddress& address, quint16 port, int minutes)
{
    QWebSocketServer server("FSRemote connectivity demo", QWebSocketServer::NonSecureMode);
    server.setProxy(QNetworkProxy::NoProxy);
    server.setHandshakeTimeout(3000);
    server.setMaxPendingConnections(8);
    int active = 0;
    QObject::connect(&server, &QWebSocketServer::newConnection, &app, [&] {
        while (server.hasPendingConnections()) {
            auto* socket = server.nextPendingConnection();
            socket->setParent(&server);
            if (active >= 8 || socket->requestUrl().path() != "/probe") {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            ++active;
            boundSocket(socket);
            const auto peer = socket->peerAddress().toString();
            log("CONNECTED " + peer);
            QObject::connect(socket, &QWebSocket::disconnected, socket, [&, socket, peer] {
                --active;
                log("DISCONNECTED " + peer);
                socket->deleteLater();
            });
            QObject::connect(socket, &QWebSocket::errorOccurred, socket,
                             [socket] { socket->abort(); });
            QObject::connect(socket, &QWebSocket::textMessageReceived, socket,
                [socket, peer, sequence = 1](const QString& message) mutable {
                    if (sequence > 5 || message != QStringLiteral("PING %1").arg(sequence)) {
                        socket->abort();
                        return;
                    }
                    log(QStringLiteral("RECEIVED PING %1 from %2; sending PONG").arg(sequence).arg(peer));
                    socket->sendTextMessage(QStringLiteral("PONG %1").arg(sequence++));
                });
            QTimer::singleShot(15000, socket, [socket] { socket->abort(); });
            socket->sendTextMessage(greeting);
        }
    });
    if (!server.listen(address, port)) {
        log("FAIL: listen: " + server.errorString());
        return 1;
    }
    log(QStringLiteral("LISTENING ws://%1:%2/probe").arg(address.toString()).arg(server.serverPort()));
    for (const auto& ip : QNetworkInterface::allAddresses()) {
        if (ip.protocol() == QAbstractSocket::IPv4Protocol && !ip.isLoopback())
            log(QStringLiteral("LOCAL ADDRESS %1").arg(ip.toString()));
    }
    log(QStringLiteral("Temporary plaintext probe only. Auto-stop in %1 minutes. Ctrl+C to stop.").arg(minutes));
    QTimer::singleShot(minutes * 60000, &app, [&] {
        log("Demo time limit reached; server stopped.");
        app.quit();
    });
    const int result = app.exec();
    // Disconnect callbacks capturing stack variables before destroying socket children.
    server.close();
    for (auto* socket : server.findChildren<QWebSocket*>()) {
        socket->disconnect();
        socket->abort();
    }
    return result;
}

int probe(QCoreApplication& app, const QUrl& url)
{
    QWebSocket socket;
    socket.setProxy(QNetworkProxy::NoProxy);
    boundSocket(&socket);
    QElapsedTimer clock;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool finished = false;
    bool ready = false;
    int sequence = 1;
    auto finish = [&](int result, const QString& message) {
        if (finished) return;
        finished = true;
        timeout.stop();
        log(message);
        socket.abort();
        app.exit(result);
    };
    QObject::connect(&timeout, &QTimer::timeout, &app, [&] {
        finish(1, "FAIL: 10-second timeout. Check server, mapping and firewall.");
    });
    QObject::connect(&socket, &QWebSocket::connected, &app, [&] {
        log("WebSocket connected. Waiting for the demo greeting.");
    });
    QObject::connect(&socket, &QWebSocket::textMessageReceived, &app, [&](const QString& message) {
        if (finished) return;
        if (!ready) {
            if (message != greeting) { finish(1, "FAIL: unexpected server greeting."); return; }
            ready = true;
            log("Server greeting verified.");
        } else {
            if (message != QStringLiteral("PONG %1").arg(sequence)) {
                finish(1, "FAIL: unexpected reply.");
                return;
            }
            log(QStringLiteral("RECEIVED PONG %1, round trip %2 ms").arg(sequence).arg(clock.elapsed()));
            if (++sequence > 5) {
                finish(0, "PASS: server greeting + 5/5 bidirectional replies verified.");
                return;
            }
        }
        clock.restart();
        log(QStringLiteral("SEND PING %1").arg(sequence));
        socket.sendTextMessage(QStringLiteral("PING %1").arg(sequence));
    });
    QObject::connect(&socket, &QWebSocket::errorOccurred, &app, [&] {
        finish(1, "FAIL: " + socket.errorString());
    });
    QObject::connect(&socket, &QWebSocket::disconnected, &app, [&] {
        if (!finished) finish(1, "FAIL: disconnected before verification completed.");
    });
    log("CONNECTING " + url.toString());
    QTimer::singleShot(0, &app, [&] { timeout.start(10000); socket.open(url); });
    return app.exec();
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("FSRemoteNetworkDemo");
    QCommandLineParser parser;
    parser.setApplicationDescription("Temporary fixed-message network probe; no credentials, chat or commands.");
    parser.addHelpOption();
    parser.addOption({"server", "Run the temporary server."});
    parser.addOption({"listen", "Numeric IPv4 listen address.", "address", "0.0.0.0"});
    parser.addOption({"port", "Listen port (0 chooses an ephemeral port).", "port", "62843"});
    parser.addOption({"minutes", "Server lifetime, 1 to 120 minutes.", "minutes", "30"});
    parser.addOption({"url", "Client endpoint, for example ws://127.0.0.1:62843/probe.", "url"});
    parser.process(app);
    if (parser.isSet("server") == parser.isSet("url")) parser.showHelp(2);
    if (parser.isSet("server")) {
        bool portOk = false;
        bool timeOk = false;
        const int port = parser.value("port").toInt(&portOk);
        const int minutes = parser.value("minutes").toInt(&timeOk);
        const QHostAddress address(parser.value("listen"));
        if (!portOk || port < 0 || port > 65535 || !timeOk || minutes < 1 || minutes > 120
            || address.protocol() != QAbstractSocket::IPv4Protocol) {
            log("FAIL: invalid listen address, port or lifetime.");
            return 2;
        }
        return serve(app, address, static_cast<quint16>(port), minutes);
    }
    const QUrl url(parser.value("url"));
    if (!url.isValid() || url.scheme() != "ws" || url.host().isEmpty()
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || url.path() != "/probe" || url.port(80) < 1) {
        log("FAIL: expected ws://host:port/probe");
        return 2;
    }
    return probe(app, url);
}
