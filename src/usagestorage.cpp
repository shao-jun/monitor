#include "usagestorage.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringLiteral>
#include <QVariant>

UsageStorage::UsageStorage() {}

UsageStorage::~UsageStorage()
{
    close();
}

bool UsageStorage::open(const QString &dbPath)
{
    m_closed = false;
    const QFileInfo fi(dbPath);
    QDir().mkpath(fi.absolutePath());

    const QString connName = QStringLiteral("usage_%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    m_db.setDatabaseName(dbPath);
    if (!m_db.open())
        return false;

    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS usage ("
        "date TEXT NOT NULL,"
        "app TEXT NOT NULL,"
        "display_name TEXT NOT NULL DEFAULT '',"
        "icon TEXT NOT NULL DEFAULT '',"
        "active_ms INTEGER NOT NULL DEFAULT 0,"
        "viewing_ms INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY (date, app))"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS locks ("
        "date TEXT PRIMARY KEY,"
        "lock_ms INTEGER NOT NULL DEFAULT 0)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS hourly_usage ("
        "date TEXT NOT NULL,"
        "hour INTEGER NOT NULL,"
        "active_ms INTEGER NOT NULL DEFAULT 0,"
        "viewing_ms INTEGER NOT NULL DEFAULT 0,"
        "lock_ms INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY (date, hour))"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS switches ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "date TEXT NOT NULL,"
        "ts INTEGER NOT NULL,"
        "kind TEXT NOT NULL DEFAULT 'window',"
        "from_app TEXT NOT NULL DEFAULT '',"
        "to_app TEXT NOT NULL DEFAULT '',"
        "from_ctx TEXT NOT NULL DEFAULT '',"
        "to_ctx TEXT NOT NULL DEFAULT '',"
        "dwell_sec INTEGER NOT NULL DEFAULT 0)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_switches_date ON switches(date, ts)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "date TEXT NOT NULL,"
        "start_ts INTEGER NOT NULL,"
        "end_ts INTEGER NOT NULL,"
        "label TEXT NOT NULL DEFAULT '')"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_events_date ON events(date, start_ts)"));
    return true;
}

bool UsageStorage::isOpen() const
{
    return m_db.isValid() && m_db.isOpen();
}

void UsageStorage::close()
{
    // 进程退出时 Qt 的 QSqlDatabase 全局表可能已经先被销毁，这里只做一次、且可重入，
    // 避免静态插件实例在 __cxa_finalize 阶段再碰数据库导致退出时段错误
    if (m_closed)
        return;
    m_closed = true;
    if (m_db.isValid()) {
        const QString name = m_db.connectionName();
        m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
}

void UsageStorage::addUsage(const QString &date, const QString &app,
                            const QString &displayName, const QString &icon,
                            const AppUsage &usage)
{
    if (!isOpen())
        return;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO usage (date, app, display_name, icon, active_ms, viewing_ms) "
        "VALUES (:date, :app, :name, :icon, :am, :vm) "
        "ON CONFLICT(date, app) DO UPDATE SET "
        "active_ms = active_ms + excluded.active_ms, "
        "viewing_ms = viewing_ms + excluded.viewing_ms, "
        "display_name = excluded.display_name, "
        "icon = excluded.icon"));
    q.bindValue(QStringLiteral(":date"), date);
    q.bindValue(QStringLiteral(":app"), app);
    q.bindValue(QStringLiteral(":name"), displayName);
    q.bindValue(QStringLiteral(":icon"), icon);
    q.bindValue(QStringLiteral(":am"), static_cast<qlonglong>(usage.activeMs));
    q.bindValue(QStringLiteral(":vm"), static_cast<qlonglong>(usage.viewingMs));
    q.exec();
}

QVector<AppUsageRow> UsageStorage::queryDay(const QString &date) const
{
    QVector<AppUsageRow> rows;
    if (!isOpen())
        return rows;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT app, display_name, icon, active_ms, viewing_ms "
        "FROM usage WHERE date = :date ORDER BY (active_ms + viewing_ms) DESC"));
    q.bindValue(QStringLiteral(":date"), date);
    if (!q.exec())
        return rows;

    while (q.next()) {
        AppUsageRow r;
        r.app = q.value(0).toString();
        r.displayName = q.value(1).toString();
        r.icon = q.value(2).toString();
        r.activeMs = q.value(3).toLongLong();
        r.viewingMs = q.value(4).toLongLong();
        rows.append(r);
    }
    return rows;
}

QVector<AppUsageRow> UsageStorage::queryMonth(const QString &yearMonth) const
{
    QVector<AppUsageRow> rows;
    if (!isOpen())
        return rows;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT app, MAX(display_name), MAX(icon), SUM(active_ms), SUM(viewing_ms) "
        "FROM usage WHERE substr(date, 1, 7) = :ym "
        "GROUP BY app ORDER BY (SUM(active_ms) + SUM(viewing_ms)) DESC"));
    q.bindValue(QStringLiteral(":ym"), yearMonth);
    if (!q.exec())
        return rows;

    while (q.next()) {
        AppUsageRow r;
        r.app = q.value(0).toString();
        r.displayName = q.value(1).toString();
        r.icon = q.value(2).toString();
        r.activeMs = q.value(3).toLongLong();
        r.viewingMs = q.value(4).toLongLong();
        rows.append(r);
    }
    return rows;
}

QVector<QPair<int, qint64>> UsageStorage::queryMonthDailyTotals(const QString &yearMonth) const
{
    QVector<QPair<int, qint64>> res;
    if (!isOpen())
        return res;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT CAST(substr(date, 9, 2) AS INTEGER) AS d, SUM(active_ms + viewing_ms) AS t "
        "FROM usage WHERE substr(date, 1, 7) = :ym GROUP BY d ORDER BY d"));
    q.bindValue(QStringLiteral(":ym"), yearMonth);
    if (!q.exec())
        return res;

    while (q.next())
        res.append(qMakePair(q.value(0).toInt(), q.value(1).toLongLong()));
    return res;
}

void UsageStorage::addLock(const QString &date, qint64 lockMs)
{
    if (!isOpen() || lockMs <= 0)
        return;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO locks (date, lock_ms) VALUES (:date, :ms) "
        "ON CONFLICT(date) DO UPDATE SET lock_ms = lock_ms + excluded.lock_ms"));
    q.bindValue(QStringLiteral(":date"), date);
    q.bindValue(QStringLiteral(":ms"), static_cast<qlonglong>(lockMs));
    q.exec();
}

qint64 UsageStorage::queryLock(const QString &date) const
{
    if (!isOpen())
        return 0;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT lock_ms FROM locks WHERE date = :date"));
    q.bindValue(QStringLiteral(":date"), date);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

qint64 UsageStorage::queryLockMonth(const QString &yearMonth) const
{
    if (!isOpen())
        return 0;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT SUM(lock_ms) FROM locks WHERE substr(date, 1, 7) = :ym"));
    q.bindValue(QStringLiteral(":ym"), yearMonth);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

void UsageStorage::addHourUsage(const QString &date, int hour, const HourlyUsage &usage)
{
    if (!isOpen())
        return;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO hourly_usage (date, hour, active_ms, viewing_ms, lock_ms) "
        "VALUES (:date, :hour, :am, :vm, :lm) "
        "ON CONFLICT(date, hour) DO UPDATE SET "
        "active_ms = active_ms + excluded.active_ms, "
        "viewing_ms = viewing_ms + excluded.viewing_ms, "
        "lock_ms = lock_ms + excluded.lock_ms"));
    q.bindValue(QStringLiteral(":date"), date);
    q.bindValue(QStringLiteral(":hour"), hour);
    q.bindValue(QStringLiteral(":am"), static_cast<qlonglong>(usage.activeMs));
    q.bindValue(QStringLiteral(":vm"), static_cast<qlonglong>(usage.viewingMs));
    q.bindValue(QStringLiteral(":lm"), static_cast<qlonglong>(usage.lockMs));
    q.exec();
}

QVector<HourlyUsageRow> UsageStorage::queryHourlyRange(const QString &startDate,
                                                       const QString &endDate) const
{
    QVector<HourlyUsageRow> rows;
    if (!isOpen())
        return rows;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT date, hour, active_ms, viewing_ms, lock_ms "
        "FROM hourly_usage WHERE date >= :s AND date <= :e ORDER BY date, hour"));
    q.bindValue(QStringLiteral(":s"), startDate);
    q.bindValue(QStringLiteral(":e"), endDate);
    if (!q.exec())
        return rows;

    while (q.next()) {
        HourlyUsageRow r;
        r.date = q.value(0).toString();
        r.hour = q.value(1).toInt();
        r.activeMs = q.value(2).toLongLong();
        r.viewingMs = q.value(3).toLongLong();
        r.lockMs = q.value(4).toLongLong();
        rows.append(r);
    }
    return rows;
}

void UsageStorage::addSwitch(const QString &date, const SwitchRow &row)
{
    if (!isOpen())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO switches (date, ts, kind, from_app, to_app, from_ctx, to_ctx, dwell_sec) "
                             "VALUES (:date, :ts, :kind, :from_app, :to_app, :from_ctx, :to_ctx, :dwell)"));
    q.bindValue(QStringLiteral(":date"), date);
    q.bindValue(QStringLiteral(":ts"), row.ts);
    q.bindValue(QStringLiteral(":kind"), row.kind);
    q.bindValue(QStringLiteral(":from_app"), row.fromApp);
    q.bindValue(QStringLiteral(":to_app"), row.toApp);
    q.bindValue(QStringLiteral(":from_ctx"), row.fromCtx);
    q.bindValue(QStringLiteral(":to_ctx"), row.toCtx);
    q.bindValue(QStringLiteral(":dwell"), row.dwellSec);
    q.exec();
}

QVector<SwitchRow> UsageStorage::switchesForDay(const QString &date) const
{
    QVector<SwitchRow> rows;
    if (!isOpen())
        return rows;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT ts, kind, from_app, to_app, from_ctx, to_ctx, dwell_sec FROM switches "
                             "WHERE date = :date ORDER BY ts ASC"));
    q.bindValue(QStringLiteral(":date"), date);
    if (!q.exec())
        return rows;
    while (q.next()) {
        SwitchRow r;
        r.ts = q.value(0).toLongLong();
        r.kind = q.value(1).toString();
        r.fromApp = q.value(2).toString();
        r.toApp = q.value(3).toString();
        r.fromCtx = q.value(4).toString();
        r.toCtx = q.value(5).toString();
        r.dwellSec = q.value(6).toInt();
        rows.append(r);
    }
    return rows;
}

int UsageStorage::addEvent(const QString &date, qint64 startTs, qint64 endTs, const QString &label)
{
    if (!isOpen())
        return -1;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO events (date, start_ts, end_ts, label) "
                             "VALUES (:date, :start, :end, :label)"));
    q.bindValue(QStringLiteral(":date"), date);
    q.bindValue(QStringLiteral(":start"), startTs);
    q.bindValue(QStringLiteral(":end"), endTs);
    q.bindValue(QStringLiteral(":label"), label);
    if (!q.exec())
        return -1;
    return q.lastInsertId().toInt();
}

QVector<DayEvent> UsageStorage::eventsForDay(const QString &date) const
{
    QVector<DayEvent> rows;
    if (!isOpen())
        return rows;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, start_ts, end_ts, label FROM events WHERE date = :date ORDER BY start_ts ASC"));
    q.bindValue(QStringLiteral(":date"), date);
    if (!q.exec())
        return rows;
    while (q.next()) {
        DayEvent e;
        e.id = q.value(0).toInt();
        e.startTs = q.value(1).toLongLong();
        e.endTs = q.value(2).toLongLong();
        e.label = q.value(3).toString();
        rows.append(e);
    }
    return rows;
}

void UsageStorage::removeEvent(int id)
{
    if (!isOpen())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM events WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
}
