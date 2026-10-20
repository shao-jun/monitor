#include "usagechart.h"

#include <QStringList>

namespace {

QString formatShort(qint64 ms)
{
    const qint64 s = ms / 1000;
    const qint64 h = s / 3600;
    const qint64 m = (s % 3600) / 60;
    if (h > 0)
        return m > 0 ? QStringLiteral("%1小时%2分").arg(h).arg(m) : QStringLiteral("%1小时").arg(h);
    if (m > 0)
        return QStringLiteral("%1分").arg(m);
    return QStringLiteral("%1秒").arg(s);
}

QString stateName(int state)
{
    switch (state) {
    case 0: return QStringLiteral("操作");
    case 1: return QStringLiteral("查看");
    case 2: return QStringLiteral("锁屏");
    default: return QStringLiteral("无记录");
    }
}

// 把连续的同类小时压成区间文字：09–11、13
QString runsOf(const QVector<int> &hours)
{
    QStringList parts;
    int i = 0;
    while (i < hours.size()) {
        int j = i;
        while (j + 1 < hours.size() && hours.at(j + 1) == hours.at(j) + 1)
            ++j;
        if (j - i >= 2)
            parts << QStringLiteral("%1–%2").arg(hours.at(i), 2, 10, QLatin1Char('0'))
                            .arg(hours.at(j), 2, 10, QLatin1Char('0'));
        else {
            for (int k = i; k <= j; ++k)
                parts << QStringLiteral("%1").arg(hours.at(k), 2, 10, QLatin1Char('0'));
        }
        i = j + 1;
    }
    return parts.join(QStringLiteral("、"));
}

} // namespace

QVector<HourlyPercent> hourlyPercents(const QVector<HourlyUsageRow> &rowsOfOneDay)
{
    QVector<HourlyPercent> res(24);
    for (const auto &r : rowsOfOneDay) {
        if (r.hour < 0 || r.hour > 23)
            continue;
        const qint64 total = r.totalMs();
        if (total <= 0)
            continue;
        HourlyPercent &p = res[r.hour];
        p.activeMin = r.activeMs / 60000.0;
        p.viewingMin = r.viewingMs / 60000.0;
        p.lockMin = r.lockMs / 60000.0;
        p.totalMin = total / 60000.0;
        p.active = r.activeMs * 100.0 / total;
        p.viewing = r.viewingMs * 100.0 / total;
        p.lock = r.lockMs * 100.0 / total;
    }
    return res;
}

int dominantState(const HourlyPercent &p)
{
    if (p.active < 0.0)
        return -1;
    if (p.active >= p.viewing && p.active >= p.lock)
        return 0;
    if (p.viewing >= p.lock)
        return 1;
    return 2;
}

QString hourlySummary(const QVector<HourlyPercent> &day)
{
    QVector<int> buckets[4];
    for (int h = 0; h < 24; ++h) {
        const int state = dominantState(day.at(h));
        buckets[state < 0 ? 3 : state].append(h);
    }

    QStringList groups;
    for (int s = 0; s < 4; ++s) {
        if (buckets[s].isEmpty())
            continue;
        // 一整天都没记录时，不必写「无记录 00–23」
        if (s == 3 && buckets[0].isEmpty() && buckets[1].isEmpty() && buckets[2].isEmpty())
            continue;
        groups << QStringLiteral("%1 %2 时").arg(stateName(s), runsOf(buckets[s]));
    }
    if (groups.isEmpty())
        return QStringLiteral("当天没有记录");
    return groups.join(QStringLiteral(" · "));
}

QString awakeSummary(const QVector<HourlyUsageRow> &rowsOfOneDay)
{
    qint64 am = 0, vm = 0;
    for (const auto &r : rowsOfOneDay) {
        am += r.activeMs;
        vm += r.viewingMs;
    }
    const qint64 awake = am + vm;
    if (awake <= 0)
        return QStringLiteral("当天没有在岗记录");
    return QStringLiteral("在岗 %1 · 操作 %2（%3%）· 查看 %4（%5%）")
        .arg(formatShort(awake), formatShort(am))
        .arg(QString::number(am * 100.0 / awake, 'f', 0))
        .arg(formatShort(vm))
        .arg(QString::number(vm * 100.0 / awake, 'f', 0));
}

QString hourDetail(const HourlyPercent &p, int hour)
{
    const QString head = QStringLiteral("%1 时").arg(hour, 2, 10, QLatin1Char('0'));
    if (p.totalMin < 0.0)
        return head + QStringLiteral(" · 无记录");
    auto mins = [](double v) { return QString::number(v, 'f', v < 10.0 ? 1 : 0); };
    return QStringLiteral("%1 · 共 %2 分（操作 %3 分 · 查看 %4 分 · 锁屏 %5 分）\n"
                          "占本小时 %6%，其中 操作 %7% · 查看 %8% · 锁屏 %9%")
        .arg(head, mins(p.totalMin), mins(p.activeMin), mins(p.viewingMin), mins(p.lockMin))
        .arg(QString::number(p.totalMin / 60.0 * 100.0, 'f', 0))
        .arg(QString::number(p.active, 'f', 0))
        .arg(QString::number(p.viewing, 'f', 0))
        .arg(QString::number(p.lock, 'f', 0));
}
