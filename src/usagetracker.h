#ifndef USAGETRACKER_H
#define USAGETRACKER_H

#include "restreminder.h"
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

    // 连续工作到点，该提醒休息了（参数：刚告一段落的连续工作时长，毫秒）
    qint64 continuousWorkMs() const { return m_rest.continuousMs(); }
    qint64 restWorkIntervalMs() const { return m_rest.workIntervalMs(); }
    // 传 0 表示关闭休息提醒；改动后连续计时清零重算
    void setRestWorkIntervalMs(qint64 ms)
    {
        m_rest.setWorkIntervalMs(ms);
        m_rest.reset();
    }

    X11WindowInfo *x11() const { return m_x11; }
    AppRegistry *registry() const { return m_registry; }

signals:
    void restReminder(qint64 workedMs);
    void lockChanged(bool locked);
    void continuousWorkChanged(qint64 workedMs);

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
    RestReminder m_rest;
};

#endif // USAGETRACKER_H
