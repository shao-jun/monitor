#include "contextswitch.h"

#include <QDateTime>
#include <QSettings>

#include "appregistry.h"
#include "chattitleocr.h"
#include "usagestorage.h"
// X11 的 Bool/None 等宏会污染 Qt 头文件，放在最后
#include "x11windowinfo.h"

namespace {
const int kOcrThrottleMs = 2000; // 企业微信里最多每 2 秒认一次字
}

ContextSwitchMonitor::ContextSwitchMonitor(QObject *parent)
    : QObject(parent)
{
    m_ocr = new ChatTitleOcr;
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("app-usage-monitor"), QStringLiteral("settings"));
    m_ocr->setEnabled(settings.value(QStringLiteral("switch/ocr"), true).toBool());
    connect(&m_timer, &QTimer::timeout, this, &ContextSwitchMonitor::tick);
}

ContextSwitchMonitor::~ContextSwitchMonitor()
{
    delete m_ocr;
}

void ContextSwitchMonitor::setX11(X11WindowInfo *x11)
{
    m_x11 = x11;
    if (m_ocr)
        m_ocr->setX11(x11);
}

void ContextSwitchMonitor::start(int intervalMs)
{
    m_timer.setInterval(intervalMs);
    m_timer.start();
}

void ContextSwitchMonitor::stop()
{
    m_timer.stop();
}

void ContextSwitchMonitor::tick()
{
    if (m_paused || !m_x11 || !m_storage)
        return;

    const X11WindowInfo::ActiveApp a = m_x11->activeApp();
    if (!a.valid)
        return;

    const QString cls = a.wmClassInstance + a.wmClass;
    if (cls.contains(QStringLiteral("dde-dock"), Qt::CaseInsensitive))
        return; // 任务栏自己的弹窗不算切换

    AppMeta meta;
    if (m_registry)
        meta = m_registry->resolve(a.wmClassInstance, a.wmClass, QString());
    QString appName = meta.name;
    if (appName.isEmpty())
        appName = !a.wmClass.isEmpty() ? a.wmClass : a.wmName;
    if (appName.isEmpty())
        return;
    // 没有 .desktop 匹配时拿到的是 wmClass（常见小写类名），显示上前缀大写更顺眼
    if (appName.size() > 1 && appName.at(0).isLower()) {
        bool ascii = true;
        for (const QChar &c : appName)
            if (c.unicode() > 127) { ascii = false; break; }
        if (ascii)
            appName[0] = appName.at(0).toUpper();
    }

    // 企业微信窗口标题恒定，只能靠截图认当前会话
    QString ctx = a.wmName;
    const bool isWxWork = cls.contains(QStringLiteral("wxwork"), Qt::CaseInsensitive)
                          || a.wmName.contains(QStringLiteral("企业微信")) || appName.contains(QStringLiteral("企业微信"));
    if (isWxWork) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        // 刚从别的应用切进来时立刻认一次，不等节流，否则这一次会被记成上一个会话
        const bool justActivated = (appName != m_lastApp);
        if (m_ocr && m_ocr->enabled() && (justActivated || nowMs - m_lastOcrAt >= kOcrThrottleMs)) {
            m_lastOcrAt = nowMs;
            const QString title = m_ocr->titleForWindow(a.window, appName);
            if (!title.isEmpty())
                m_cachedChat = title;
        }
        if (!m_cachedChat.isEmpty())
            ctx = m_cachedChat;
    } else {
        m_cachedChat.clear();
    }
    if (ctx.isEmpty())
        ctx = appName;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString key = appName + QChar(0x1f) + ctx;

    if (m_lastKey.isEmpty()) {
        m_lastKey = key;
        m_lastApp = appName;
        m_lastCtx = ctx;
        m_lastSince = now;
        return;
    }
    if (key == m_lastKey)
        return;

    SwitchRow row;
    row.ts = now;
    // 只有企业微信里换会话才算 chat；别的应用换了标题（SmartGit 换仓库、浏览器换页）仍是窗口切换，
    // 否则「企业微信会话 Top」里会混进一堆无关的标题
    row.kind = (appName == m_lastApp && isWxWork) ? QStringLiteral("chat") : QStringLiteral("window");
    row.fromApp = m_lastApp;
    row.toApp = appName;
    row.fromCtx = m_lastCtx;
    row.toCtx = ctx;
    row.dwellSec = int(qMax<qint64>(0, now - m_lastSince));
    m_storage->addSwitch(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")), row);

    m_lastKey = key;
    m_lastApp = appName;
    m_lastCtx = ctx;
    m_lastSince = now;
    emit switched();
}
