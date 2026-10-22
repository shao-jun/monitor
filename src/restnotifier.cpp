#include "restnotifier.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDebug>
#include <QStringList>
#include <QVariantMap>

RestNotifier::RestNotifier(QObject *parent)
    : QObject(parent)
{
}

QDBusInterface *RestNotifier::interface()
{
    if (!m_iface) {
        m_iface = new QDBusInterface(QStringLiteral("org.freedesktop.Notifications"),
                                     QStringLiteral("/org/freedesktop/Notifications"),
                                     QStringLiteral("org.freedesktop.Notifications"),
                                     QDBusConnection::sessionBus(), this);
    }
    return m_iface->isValid() ? m_iface : nullptr;
}

void RestNotifier::notifyRest(int workedMinutes, const QString &iconPath)
{
    QDBusInterface *iface = interface();
    if (!iface)
        return;

    const QString summary = QStringLiteral("该休息一下了");
    const QString body = QStringLiteral("已连续工作 %1 分钟。站起来活动活动，看看远处，让眼睛歇一会儿。")
                             .arg(workedMinutes);

    QVariantMap hints;
    hints.insert(QStringLiteral("urgency"), QVariant::fromValue<uchar>(1)); // 1 = normal
    hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("app-usage-monitor"));

    const QDBusMessage reply = iface->call(QStringLiteral("Notify"),
                                           QStringLiteral("app-usage-monitor"), // app_name
                                           0u,                                  // replaces_id
                                           iconPath,                            // app_icon
                                           summary,
                                           body,
                                           QStringList(),                       // actions
                                           hints,
                                           8000);                               // 气泡停留毫秒

    if (reply.type() == QDBusMessage::ErrorMessage)
        qWarning() << "[rest-reminder] 通知发送失败:" << reply.errorMessage();
}
