#ifndef RESTREMINDER_H
#define RESTREMINDER_H

#include <QtGlobal>

// 连续工作计时与休息提醒判定。
//
// 规则（与产品约定一致）：
//   * 「操作」和「查看」都算在工作，两者累加；
//   * 连续工作满 30 分钟触发一次提醒，触发后从 0 重新计时；
//   * 锁屏累计满 3 分钟视为一次中断，连续工作时间清零；
//     不足 3 分钟（例如临时锁屏再解开）不打断连续计时，锁屏时长本身也不计入工作。
//
// 纯计算，不读系统时钟，全部时长由调用方喂进来，方便单元测试。
class RestReminder
{
public:
    static constexpr qint64 kWorkIntervalMs = 30 * 60 * 1000; // 连续工作 30 分钟提醒
    static constexpr qint64 kLockBreakMs = 3 * 60 * 1000;     // 锁屏 3 分钟算中断

    // 记入一段工作（操作或查看）时长。
    // 返回 true 表示这一刻该提醒休息了：内部已扣掉一个完整周期，接着计下一段。
    bool addWork(qint64 ms);

    // 记入一段锁屏时长。
    // 返回 true 表示这次锁屏把连续工作时间清零了（即真正构成一次中断）。
    bool addLock(qint64 ms);

    // 锁屏结束（只清锁屏累计，连续工作时间按阈值是否已达决定保留或已清零）。
    void endLock();

    // 默认 30 分钟；传给 0 或负数表示关闭提醒
    void setWorkIntervalMs(qint64 ms)
    {
        m_workIntervalMs = ms;
        if (ms <= 0)
            m_workMs = 0;
    }
    qint64 workIntervalMs() const { return m_workIntervalMs; }
    bool enabled() const { return m_workIntervalMs > 0; }

    qint64 continuousMs() const { return m_workMs; }
    qint64 lockMs() const { return m_lockMs; }

    void reset();

private:
    qint64 m_workMs = 0;
    qint64 m_lockMs = 0;
    qint64 m_workIntervalMs = kWorkIntervalMs;
};

#endif // RESTREMINDER_H
