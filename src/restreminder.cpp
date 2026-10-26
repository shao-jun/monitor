#include "restreminder.h"

// C++11 需要在类外给出 constexpr 静态成员的定义（取地址/引用时才会真正用到）
constexpr qint64 RestReminder::kWorkIntervalMs;
constexpr qint64 RestReminder::kLockBreakMs;

bool RestReminder::addWork(qint64 ms)
{
    if (ms <= 0 || m_workIntervalMs <= 0)
        return false;

    m_workMs += ms;
    if (m_workMs < m_workIntervalMs)
        return false;

    // 一个采样周期最多提前几秒触发，扣掉一个完整周期即可，余数留给下一段
    m_workMs -= m_workIntervalMs;
    if (m_workMs < 0)
        m_workMs = 0;
    return true;
}

bool RestReminder::addLock(qint64 ms)
{
    if (ms <= 0)
        return false;

    m_lockMs += ms;
    if (m_lockMs < kLockBreakMs)
        return false;

    const bool interrupted = m_workMs > 0;
    m_workMs = 0;
    return interrupted;
}

void RestReminder::endLock()
{
    m_lockMs = 0;
}

void RestReminder::reset()
{
    m_workMs = 0;
    m_lockMs = 0;
}
