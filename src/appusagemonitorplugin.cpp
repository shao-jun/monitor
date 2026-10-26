#include "appusagemonitorplugin.h"

#include "usagestorage.h"
#include "usagetracker.h"
#include "usagepopupwidget.h"
#include "restnotifier.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
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

    // 恢复上次设置的休息提醒间隔
    m_tracker->setRestWorkIntervalMs(qint64(restIntervalMinutes()) * 60 * 1000);
    m_lastSettingsMtime = settingsMtime();

    // 配置文件被改了就即时生效，不需要重启任务栏
    m_settingsTimer = new QTimer(this);
    m_settingsTimer->setInterval(5000);
    connect(m_settingsTimer, &QTimer::timeout, this, &AppUsageMonitorPlugin::reloadRestIntervalIfChanged);
    m_settingsTimer->start();
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

int AppUsageMonitorPlugin::restIntervalMinutes() const
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("app-usage-monitor"), QStringLiteral("settings"));
    // 0 表示关闭提醒，默认 30 分钟
    return settings.value(QStringLiteral("rest/intervalMinutes"), 30).toInt();
}

void AppUsageMonitorPlugin::applyRestInterval(int minutes, bool persist)
{
    if (persist) {
        QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                           QStringLiteral("app-usage-monitor"), QStringLiteral("settings"));
        settings.setValue(QStringLiteral("rest/intervalMinutes"), minutes);
        settings.sync();
    }
    m_tracker->setRestWorkIntervalMs(qint64(minutes) * 60 * 1000);
    if (persist)
        m_lastSettingsMtime = settingsMtime();
    onContinuousWorkChanged(m_tracker->continuousWorkMs());
}

qint64 AppUsageMonitorPlugin::currentRestIntervalMs() const
{
    return m_tracker->restWorkIntervalMs();
}

qint64 AppUsageMonitorPlugin::settingsMtime() const
{
    const QFileInfo info(QSettings(QSettings::IniFormat, QSettings::UserScope,
                                   QStringLiteral("app-usage-monitor"),
                                   QStringLiteral("settings")).fileName());
    return info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0;
}

void AppUsageMonitorPlugin::reloadRestIntervalIfChanged()
{
    const qint64 mtime = settingsMtime();
    if (mtime == m_lastSettingsMtime)
        return;

    m_lastSettingsMtime = mtime;
    const int minutes = restIntervalMinutes();
    m_tracker->setRestWorkIntervalMs(qint64(minutes) * 60 * 1000);
    qInfo("app-usage-monitor: 休息提醒间隔已更新为 %d 分钟（0=关闭）", minutes);
    onContinuousWorkChanged(m_tracker->continuousWorkMs());
}

const QString AppUsageMonitorPlugin::itemContextMenu(const QString &itemKey)
{
    Q_UNUSED(itemKey);

    const int current = restIntervalMinutes();
    QJsonArray items;

    const QVector<int> choices{20, 30, 45, 60};
    for (int minutes : choices) {
        QJsonObject item;
        item[QStringLiteral("itemId")] = QStringLiteral("rest-%1").arg(minutes);
        item[QStringLiteral("itemText")] = QStringLiteral("连续工作 %1 分钟提醒").arg(minutes);
        item[QStringLiteral("isActive")] = true;
        item[QStringLiteral("isCheckable")] = true;
        item[QStringLiteral("checked")] = (current == minutes);
        items.append(item);
    }

    QJsonObject off;
    off[QStringLiteral("itemId")] = QStringLiteral("rest-off");
    off[QStringLiteral("itemText")] = QStringLiteral("关闭休息提醒");
    off[QStringLiteral("isActive")] = true;
    off[QStringLiteral("isCheckable")] = true;
    off[QStringLiteral("checked")] = (current <= 0);
    items.append(off);

    QJsonObject menu;
    menu[QStringLiteral("items")] = items;
    menu[QStringLiteral("checkableMenu")] = false;
    menu[QStringLiteral("singleCheck")] = false;
    return QString::fromUtf8(QJsonDocument(menu).toJson(QJsonDocument::Compact));
}

void AppUsageMonitorPlugin::invokedMenuItem(const QString &itemKey, const QString &menuId, const bool checked)
{
    Q_UNUSED(itemKey);
    Q_UNUSED(checked);

    if (menuId == QLatin1String("rest-off")) {
        applyRestInterval(0, true);
        return;
    }
    if (menuId.startsWith(QLatin1String("rest-"))) {
        bool ok = false;
        const int minutes = menuId.mid(5).toInt(&ok);
        if (ok && minutes > 0)
            applyRestInterval(minutes, true);
    }
}
