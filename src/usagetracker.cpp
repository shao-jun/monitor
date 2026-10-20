#include "usagetracker.h"

#include <QDBusConnection>
#include <QDBusInterface>

#include "appregistry.h"
#include "idledetector.h"
#include "usagestorage.h"
#include "x11windowinfo.h"

#include <QDate>
#include <QDateTime>
#include <QFile>

#include <unistd.h>

namespace {
const qint64 kAutoFlushIntervalMs = 10000;
}

static QString processNameOf(const X11WindowInfo::ActiveApp &a)
{
    if (a.pid <= 0)
        return QString();
    QFile f(QStringLiteral("/proc/%1/comm").arg(a.pid));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray b = f.readAll().trimmed();
    f.close();
    return QString::fromLocal8Bit(b);
}

UsageTracker::UsageTracker(QObject *parent)
    : QObject(parent)
    , m_x11(new X11WindowInfo)
    , m_idle(new IdleDetector(this))
    , m_registry(new AppRegistry)
{
    m_x11->init();
    m_idle->setParent(this);
}

UsageTracker::~UsageTracker()
{
    flushNow();
    delete m_x11;
    delete m_registry;
}

void UsageTracker::setStorage(UsageStorage *storage)
{
    m_storage = storage;
}

void UsageTracker::setRegistry(AppRegistry *registry)
{
    delete m_registry;
    m_registry = registry;
}

void UsageTracker::start(int intervalMs)
{
    m_intervalMs = intervalMs;
    m_flushCounter = 0;
    connect(&m_timer, &QTimer::timeout, this, &UsageTracker::tick);
    m_timer.start(m_intervalMs);
}

void UsageTracker::stop()
{
    m_timer.stop();
    flushNow();
}

void UsageTracker::tick()
{
    if (!m_storage || !m_idle || !m_registry)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 idle = static_cast<qint64>(m_idle->idleMilliseconds());

    const QDateTime startDt = QDateTime::fromMSecsSinceEpoch(m_lastTickMs);
    const QString startDate = startDt.date().toString(QStringLiteral("yyyy-MM-dd"));
    const int startHour = startDt.time().hour();

    if (isLocked()) {
        const qint64 dt = now - m_lastTickMs;
        if (!m_locked) {
            m_locked = true;
            flushNow();
        } else if (dt > 0) {
            m_lockBuffer[QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))] += dt;
            m_hourBuffer[startDate][startHour].lockMs += dt;
        }
        m_lastTickMs = now;
        m_lastIdleMs = idle;
        m_flushCounter += dt > 0 ? dt : 0;
        if (m_flushCounter >= kAutoFlushIntervalMs)
            flushNow();
        return;
    }
    m_locked = false;

    auto active = m_x11->activeApp();

    if (active.valid && active.pid == static_cast<pid_t>(::getpid()))
        active.valid = false;

    const QString procName = processNameOf(active);
    QString appId = m_lastApp;
    QString dispName;
    QString icon;
    if (active.valid) {
        appId = active.wmClassInstance.isEmpty() ? procName : active.wmClassInstance;
        appId = appId.trimmed().toLower();
        if (appId.isEmpty())
            appId = active.wmClass.toLower();

        const AppMeta meta = m_registry->resolve(active.wmClassInstance, active.wmClass, procName);
        dispName = meta.name;
        icon = meta.icon;
        m_metaCache[appId] = qMakePair(dispName, icon);
    }

    const QString date = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));

    if (m_first) {
        m_first = false;
        m_lastTickMs = now;
        m_lastIdleMs = idle;
        m_lastApp = appId;
        m_lastDate = date;
        return;
    }

    qint64 dt = now - m_lastTickMs;
    const qint64 maxDt = static_cast<qint64>(m_intervalMs) * 4;
    if (dt > maxDt)
        dt = maxDt;
    if (dt < 0)
        dt = 0;

    if (!m_lastApp.isEmpty() && dt > 0) {
        const qint64 deltaIdle = idle - m_lastIdleMs;
        bool viewing;
        if (deltaIdle < 0) {
            viewing = false;
        } else if (deltaIdle >= dt * 3 / 5) {
            viewing = true;
        } else {
            viewing = false;
        }
        AppUsage u;
        if (viewing)
            u.viewingMs = dt;
        else
            u.activeMs = dt;
        m_buffer[m_lastDate][m_lastApp] += u;

        HourlyUsage h;
        if (viewing)
            h.viewingMs = dt;
        else
            h.activeMs = dt;
        m_hourBuffer[m_lastDate][startHour] += h;
    }

    m_lastTickMs = now;
    m_lastIdleMs = idle;
    m_lastApp = appId;
    m_lastDate = date;

    m_flushCounter += dt;
    if (m_flushCounter >= kAutoFlushIntervalMs)
        flushNow();
}

bool UsageTracker::isLocked()
{
    if (!m_lockIface) {
        m_lockIface = new QDBusInterface(
            QStringLiteral("com.deepin.SessionManager"),
            QStringLiteral("/com/deepin/SessionManager"),
            QStringLiteral("com.deepin.SessionManager"),
            QDBusConnection::sessionBus(),
            this);
    }
    if (!m_lockIface->isValid())
        return false;
    return m_lockIface->property("Locked").toBool();
}

void UsageTracker::flushNow()
{
    m_flushCounter = 0;
    if (!m_storage)
        return;

    for (auto itDate = m_buffer.constBegin(); itDate != m_buffer.constEnd(); ++itDate) {
        const QString &date = itDate.key();
        const QMap<QString, AppUsage> &apps = itDate.value();
        for (auto itApp = apps.constBegin(); itApp != apps.constEnd(); ++itApp) {
            const QString &app = itApp.key();
            const AppUsage &u = itApp.value();
            if (u.totalMs() <= 0)
                continue;
            QString name = app;
            QString icon;
            const auto mit = m_metaCache.constFind(app);
            if (mit != m_metaCache.constEnd()) {
                name = mit->first;
                icon = mit->second;
            }
            m_storage->addUsage(date, app, name, icon, u);
        }
    }
    m_buffer.clear();

    for (auto it = m_lockBuffer.constBegin(); it != m_lockBuffer.constEnd(); ++it) {
        if (it.value() > 0)
            m_storage->addLock(it.key(), it.value());
    }
    m_lockBuffer.clear();

    for (auto itDate = m_hourBuffer.constBegin(); itDate != m_hourBuffer.constEnd(); ++itDate) {
        const QString &date = itDate.key();
        const QMap<int, HourlyUsage> &hours = itDate.value();
        for (auto itHour = hours.constBegin(); itHour != hours.constEnd(); ++itHour) {
            const HourlyUsage &u = itHour.value();
            if (u.totalMs() <= 0)
                continue;
            m_storage->addHourUsage(date, itHour.key(), u);
        }
    }
    m_hourBuffer.clear();
}
