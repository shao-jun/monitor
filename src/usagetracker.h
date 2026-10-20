#ifndef USAGETRACKER_H
#define USAGETRACKER_H

#include "usageentry.h"

#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTimer>

class X11WindowInfo;
class IdleDetector;
class AppRegistry;
class UsageStorage;
class QDBusInterface;

class UsageTracker : public QObject
{
    Q_OBJECT
public:
    explicit UsageTracker(QObject *parent = nullptr);
    ~UsageTracker() override;

    void setStorage(UsageStorage *storage);
    void setRegistry(AppRegistry *registry);

    void start(int intervalMs = 3000);
    void stop();

    UsageStorage *storage() const { return m_storage; }

public slots:
    void flushNow();

private slots:
    void tick();

private:
    bool isLocked();

    X11WindowInfo *m_x11 = nullptr;
    IdleDetector *m_idle = nullptr;
    AppRegistry *m_registry = nullptr;
    UsageStorage *m_storage = nullptr;
    QDBusInterface *m_lockIface = nullptr;
    bool m_locked = false;
    QTimer m_timer;

    int m_intervalMs = 3000;
    qint64 m_flushCounter = 0;

    qint64 m_lastTickMs = 0;
    qint64 m_lastIdleMs = 0;
    QString m_lastApp;
    QString m_lastDate;
    bool m_first = true;

    QMap<QString, QMap<QString, AppUsage>> m_buffer;
    QMap<QString, qint64> m_lockBuffer;
    QMap<QString, QMap<int, HourlyUsage>> m_hourBuffer;
    QMap<QString, QPair<QString, QString>> m_metaCache;
};

#endif // USAGETRACKER_H
