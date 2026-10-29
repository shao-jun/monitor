#ifndef SWITCHSTATS_H
#define SWITCHSTATS_H

#include "usageentry.h"

#include <QPair>
#include <QString>
#include <QVector>

// 一天的上下文切换统计（纯计算，方便单测）
struct SwitchStats {
    int total = 0;
    int fragmented = 0;                            // 上一个上下文停留 < 10 秒的“碎片切换”
    QVector<int> perHour;                          // 24 个小时里各自发生多少次切换
    QVector<QPair<QString, int>> topApps;         // 每个应用被切进去的次数："Hermes 12"
    QVector<QPair<QString, int>> topTransitions;   // 跨应用跳转，企业微信那侧写具体会话："Hermes → 高维舟"
    QVector<QPair<QString, int>> topChats;         // 企业微信会话名（被切进去的次数）
    double avgIntervalMin = 0.0;                   // 平均多少分钟切一次
    int peakHour = -1;
    int peakCount = 0;
    qint64 firstTs = 0;
    qint64 lastTs = 0;
};

SwitchStats computeSwitchStats(const QVector<SwitchRow> &rows);

#endif // SWITCHSTATS_H
