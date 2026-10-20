#ifndef USAGECHART_H
#define USAGECHART_H

#include "usageentry.h"

#include <QString>
#include <QVector>

// 单日 24 小时的构成。所有 *_min 都是「分钟数」，直接能跟一小时 60 分钟对比：
// 只过了 17 分钟的那一小时就只能画出 17 分钟的柱子，不会被放大成 100%。
// 百分比字段只用于悬停提示（占该小时已有记录的比例），无记录小时为 -1。
struct HourlyPercent
{
    double activeMin = 0.0;
    double viewingMin = 0.0;
    double lockMin = 0.0;
    double totalMin = -1.0;   // 该小时已记录的总分钟数，-1 表示无记录
    double active = -1.0;
    double viewing = -1.0;
    double lock = -1.0;
};

QVector<HourlyPercent> hourlyPercents(const QVector<HourlyUsageRow> &rowsOfOneDay);

// 某一小时的主导状态：0=操作 1=查看 2=锁屏 -1=无记录
int dominantState(const HourlyPercent &p);

// 按主导状态把一天切分成时段，例如：
// 「操作 09–11、13–16 时 · 查看 10、12 时 · 锁屏 00–08、17–23 时」
QString hourlySummary(const QVector<HourlyPercent> &day);

// 「在岗 6小时20分 · 操作 4小时50分（76%）· 查看 1小时30分（24%）」
QString awakeSummary(const QVector<HourlyUsageRow> &rowsOfOneDay);

// 悬停用的一行数值：
// 「09 时 · 共 17 分 · 操作 12 分 · 查看 3 分 · 锁屏 2 分（操作 71% · 查看 18% · 锁屏 12%）」
QString hourDetail(const HourlyPercent &p, int hour);

#endif // USAGECHART_H
