#include "appusagemonitorplugin.h"

#include "usagestorage.h"
#include "usagetracker.h"
#include "usagepopupwidget.h"
#include "restnotifier.h"

#include <QDir>
#include <QStandardPaths>
#include <QLabel>
#include <QPainter>
#include <QPixmap>

static QPixmap drawDockPixmap(int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c(0x2a, 0x82, 0xda);
    const int barW = size / 5;
    const int gap = size / 10;
    const int heights[3] = { size * 2 / 5, size * 3 / 4, size * 1 / 2 };
    int x = size / 8;
    for (int i = 0; i < 3; ++i) {
        p.fillRect(x, size - heights[i], barW, heights[i], c);
        x += barW + gap;
    }
    return pm;
}

AppUsageMonitorPlugin::AppUsageMonitorPlugin(QObject *parent)
    : QObject(parent)
    , m_tracker(new UsageTracker(this))
    , m_storage(new UsageStorage)
{
    m_tracker->setStorage(m_storage);
    m_notifier = new RestNotifier(this);
    connect(m_tracker, &UsageTracker::restReminder, this, &AppUsageMonitorPlugin::onRestReminder);
    connect(m_tracker, &UsageTracker::continuousWorkChanged, this,
            &AppUsageMonitorPlugin::onContinuousWorkChanged);

    const QString dbPath = QDir::home().filePath(QStringLiteral(".local/share/app-usage-monitor/usage.db"));
    m_storage->open(dbPath);
}

AppUsageMonitorPlugin::~AppUsageMonitorPlugin()
{
    if (m_tracker) {
        m_tracker->stop();
        m_tracker->setStorage(nullptr);
    }
    delete m_storage;
}

const QString AppUsageMonitorPlugin::pluginName() const
{
    return QStringLiteral("app-usage-monitor");
}

const QString AppUsageMonitorPlugin::pluginDisplayName() const
{
    return QStringLiteral("应用使用时间");
}

void AppUsageMonitorPlugin::init(PluginProxyInterface *proxyInter)
{
    m_proxyInter = proxyInter;
    m_itemKey = pluginName();
    m_proxyInter->itemAdded(this, m_itemKey);
    m_tracker->start(3000);
}

QWidget *AppUsageMonitorPlugin::itemWidget(const QString &itemKey)
{
    Q_UNUSED(itemKey);
    if (!m_itemLabel) {
        m_itemLabel = new QLabel;
        m_itemLabel->setPixmap(drawDockPixmap(16));
        m_itemLabel->setFixedSize(16, 16);
    }
    return m_itemLabel;
}

QWidget *AppUsageMonitorPlugin::itemTipsWidget(const QString &itemKey)
{
    Q_UNUSED(itemKey);
    if (!m_tips) {
        m_tips = new QLabel;
        onContinuousWorkChanged(m_tracker ? m_tracker->continuousWorkMs() : 0);
    }
    return m_tips;
}

void AppUsageMonitorPlugin::onRestReminder(qint64 workedMs)
{
    m_notifier->notifyRest(int(workedMs / 60000), notificationIconPath());
}

void AppUsageMonitorPlugin::onContinuousWorkChanged(qint64 workedMs)
{
    if (!m_tips)
        return;
    const int minutes = int(workedMs / 60000);
    if (minutes > 0) {
        m_tips->setText(QStringLiteral("应用使用时间 · 已连续工作 %1 分钟").arg(minutes));
    } else {
        m_tips->setText(QStringLiteral("应用使用时间"));
    }
}

// 气泡图标：把任务栏那个三柱图标存成临时文件，通知服务按路径取用
QString AppUsageMonitorPlugin::notificationIconPath()
{
    if (!m_notifyIcon.isEmpty())
        return m_notifyIcon;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString path = QDir(dir).filePath(QStringLiteral("app-usage-monitor-notify.png"));
    if (drawDockPixmap(48).save(path, "PNG"))
        m_notifyIcon = path;
    return m_notifyIcon;
}

QWidget *AppUsageMonitorPlugin::itemPopupApplet(const QString &itemKey)
{
    Q_UNUSED(itemKey);
    if (!m_popup) {
        m_popup = new UsagePopupWidget(m_storage);
        connect(m_popup, &UsagePopupWidget::opened, this, [this]() {
            m_tracker->flushNow();
            m_popup->reload();
        });
    }
    return m_popup;
}

Dock::PluginFlags AppUsageMonitorPlugin::flags() const
{
    return Dock::Type_Tray | Dock::Attribute_Normal;
}

QIcon AppUsageMonitorPlugin::icon(Dock::IconType, Dock::ThemeType) const
{
    return QIcon(drawDockPixmap(16));
}
