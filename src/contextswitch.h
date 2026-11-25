#ifndef CONTEXTSWITCH_H
#define CONTEXTSWITCH_H

#include <QObject>
#include <QString>
#include <QTimer>

// 切换类型与停留时长截断：抽成纯函数，offscreen 单测直接调，不用起 X
namespace SwitchRule {
enum Kind { Window = 0, Inner = 1, Chat = 2 };
// 同一应用内：企业微信算 chat（换会话），其他应用算 inner（换标签/换目录）
inline Kind kindFor(bool sameApp, bool wxwork)
{
    if (!sameApp)
        return Window;
    return wxwork ? Chat : Inner;
}
inline const char *kindName(Kind k)
{
    return k == Chat ? "chat" : (k == Inner ? "inner" : "window");
}
// 休眠/关机回来时 gap 可能是几十小时，不截断会造出「停留 64 小时」
inline int clampDwellSec(qint64 gapSec)
{
    const qint64 kMax = 20 * 60;
    return static_cast<int>(gapSec < 0 ? 0 : (gapSec > kMax ? kMax : gapSec));
}
}

class X11WindowInfo;
class AppRegistry;
class UsageStorage;
class ChatTitleOcr;

// 盯着前台窗口，上下文变了就记一条：换应用/换窗口，或企业微信里换了会话
class ContextSwitchMonitor : public QObject
{
    Q_OBJECT
public:
    explicit ContextSwitchMonitor(QObject *parent = nullptr);
    ~ContextSwitchMonitor() override;

    void setStorage(UsageStorage *storage) { m_storage = storage; }
    void setRegistry(AppRegistry *registry) { m_registry = registry; }
    void setX11(X11WindowInfo *x11);

    void start(int intervalMs = 1000);
    void stop();
    void setPaused(bool paused) { m_paused = paused; }
    bool paused() const { return m_paused; }

    ChatTitleOcr *ocr() const { return m_ocr; }

signals:
    void switched();

private slots:
    void tick();

private:
    X11WindowInfo *m_x11 = nullptr;
    AppRegistry *m_registry = nullptr;
    UsageStorage *m_storage = nullptr;
    ChatTitleOcr *m_ocr = nullptr;
    QTimer m_timer;

    QString m_lastKey;
    QString m_candidateKey;
    QString m_lastApp;
    QString m_lastCtx;
    qint64 m_lastSince = 0;
    qint64 m_lastOcrAt = 0;
    QString m_cachedChat;
    bool m_paused = false;
};

#endif // CONTEXTSWITCH_H
