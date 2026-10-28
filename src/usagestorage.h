#ifndef USAGESTORAGE_H
#define USAGESTORAGE_H

#include "usageentry.h"

#include <QSqlDatabase>
#include <QString>
#include <QVector>

class UsageStorage
{
public:
    UsageStorage();
    ~UsageStorage();

    bool open(const QString &dbPath);
    bool isOpen() const;
    void close();

    void addUsage(const QString &date, const QString &app,
                  const QString &displayName, const QString &icon,
                  const AppUsage &usage);

    void addLock(const QString &date, qint64 lockMs);
    qint64 queryLock(const QString &date) const;
    qint64 queryLockMonth(const QString &yearMonth) const;

    void addHourUsage(const QString &date, int hour, const HourlyUsage &usage);
    QVector<HourlyUsageRow> queryHourlyRange(const QString &startDate, const QString &endDate) const;

    QVector<AppUsageRow> queryDay(const QString &date) const;
    QVector<AppUsageRow> queryMonth(const QString &yearMonth) const;
    QVector<QPair<int, qint64>> queryMonthDailyTotals(const QString &yearMonth) const;

    // ---- 上下文切换 ----
    void addSwitch(const QString &date, const SwitchRow &row);
    QVector<SwitchRow> switchesForDay(const QString &date) const;

    // ---- 手动事件（出去开会等）----
    int addEvent(const QString &date, qint64 startTs, qint64 endTs, const QString &label);
    QVector<DayEvent> eventsForDay(const QString &date) const;
    void removeEvent(int id);

private:
    QSqlDatabase m_db;
};

#endif // USAGESTORAGE_H
