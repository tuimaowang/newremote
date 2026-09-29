#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

namespace protocol {
inline constexpr int maxWireBytes = 16384;
inline constexpr int maxTextLength = 4000;
inline constexpr qint64 maxQueuedBytes = 256 * 1024;

inline QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
inline QJsonObject message(const QString& type, QJsonObject payload = {})
{
    return {{"version", 1}, {"id", id()}, {"type", type}, {"payload", payload}};
}
inline QString encode(const QJsonObject& value)
{
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}
inline bool validId(const QString& value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,80}$"));
    return pattern.match(value).hasMatch();
}
inline bool decode(const QString& text, QJsonObject* value)
{
    if (text.toUtf8().size() > maxWireBytes) return false;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    *value = doc.object();
    return value->value("version").toDouble() == 1.0
        && validId(value->value("id").toString())
        && value->value("type").isString()
        && value->value("type").toString().size() <= 64
        && value->value("payload").isObject();
}
}

