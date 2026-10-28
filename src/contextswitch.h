#ifndef CONTEXTSWITCH_H
#define CONTEXTSWITCH_H

#include <QObject>
#include <QString>
#include <QTimer>

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
    QString m_lastApp;
    QString m_lastCtx;
    qint64 m_lastSince = 0;
    qint64 m_lastOcrAt = 0;
    QString m_cachedChat;
    bool m_paused = false;
};

#endif // CONTEXTSWITCH_H
