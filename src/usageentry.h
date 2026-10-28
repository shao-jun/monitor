#ifndef USAGEENTRY_H
#define USAGEENTRY_H

#include <QtGlobal>
#include <QString>

// 一个应用在某一天的使用统计
struct AppUsage {
    qint64 activeMs = 0;   // 操作时间（打字/鼠标，idle 不增长）
    qint64 viewingMs = 0;  // 查看时间（无输入，idle 随时间增长）

    qint64 totalMs() const { return activeMs + viewingMs; }
    AppUsage &operator+=(const AppUsage &o) {
        activeMs += o.activeMs;
        viewingMs += o.viewingMs;
        return *this;
    }
};

// 查询结果中的一行
struct AppUsageRow {
    QString app;         // 稳定标识
    QString displayName; // 展示名
    QString icon;        // 图标名或路径
    qint64 activeMs = 0;
    qint64 viewingMs = 0;

    qint64 totalMs() const { return activeMs + viewingMs; }
};

// 某一天某个小时的使用统计（操作 / 查看 / 锁屏）
struct HourlyUsage {
    qint64 activeMs = 0;   // 操作时间
    qint64 viewingMs = 0;  // 查看时间
    qint64 lockMs = 0;     // 锁屏时间

    qint64 totalMs() const { return activeMs + viewingMs + lockMs; }
    HourlyUsage &operator+=(const HourlyUsage &o) {
        activeMs += o.activeMs;
        viewingMs += o.viewingMs;
        lockMs += o.lockMs;
        return *this;
    }
};

// 按小时查询结果中的一行
struct HourlyUsageRow {
    QString date;      // yyyy-MM-dd
    int hour = 0;      // 0-23
    qint64 activeMs = 0;
    qint64 viewingMs = 0;
    qint64 lockMs = 0;

    qint64 totalMs() const { return activeMs + viewingMs + lockMs; }
};

// 一次上下文切换（窗口切换或企业微信会话切换）
struct SwitchRow {
    qint64 ts = 0;          // 发生时间（秒）
    QString kind;           // "window" 换窗口 / "chat" 同一应用内换会话
    QString fromApp;
    QString toApp;
    QString fromCtx;        // 上一个上下文（窗口标题或会话名）
    QString toCtx;
    int dwellSec = 0;       // 上一个上下文停留了多久
};

// 手动添加的事件（出去开会、别人叫走等）
struct DayEvent {
    int id = 0;
    qint64 startTs = 0;
    qint64 endTs = 0;
    QString label;
};

#endif // USAGEENTRY_H
