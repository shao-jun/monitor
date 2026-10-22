#ifndef RESTNOTIFIER_H
#define RESTNOTIFIER_H

#include <QObject>
#include <QString>

class QDBusInterface;

// 通过 org.freedesktop.Notifications 弹一条桌面气泡（走系统的通知中心）
class RestNotifier : public QObject
{
    Q_OBJECT
public:
    explicit RestNotifier(QObject *parent = nullptr);

    // iconPath 可为空；空时由通知服务按应用名给默认图标
    void notifyRest(int workedMinutes, const QString &iconPath);

private:
    QDBusInterface *interface();

    QDBusInterface *m_iface = nullptr;
};

#endif // RESTNOTIFIER_H
