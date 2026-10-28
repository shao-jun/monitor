#ifndef APPUSAGEMONITORPLUGIN_H
#define APPUSAGEMONITORPLUGIN_H

#include "pluginsiteminterface_v2.h"

#include <QObject>

class UsageTracker;
class ContextSwitchMonitor;
class UsagePopupWidget;
class UsageStorage;
class QLabel;
class QTimer;
class RestNotifier;

class AppUsageMonitorPlugin : public QObject, public PluginsItemInterfaceV2
{
    Q_OBJECT
    Q_INTERFACES(PluginsItemInterfaceV2 PluginsItemInterface)
    Q_PLUGIN_METADATA(IID "com.deepin.dock.PluginsItemInterface_V2" FILE "app-usage-monitor.json")

public:
    explicit AppUsageMonitorPlugin(QObject *parent = nullptr);
    ~AppUsageMonitorPlugin() override;

    const QString pluginName() const override;
    const QString pluginDisplayName() const override;
    void init(PluginProxyInterface *proxyInter) override;
    QWidget *itemWidget(const QString &itemKey) override;
    QWidget *itemTipsWidget(const QString &itemKey) override;
    QWidget *itemPopupApplet(const QString &itemKey) override;
    const QString itemContextMenu(const QString &itemKey) override;
    void invokedMenuItem(const QString &itemKey, const QString &menuId, const bool checked) override;

    // 供测试使用：当前生效的休息提醒间隔（毫秒，0=关闭）
    qint64 currentRestIntervalMs() const;

    Dock::PluginFlags flags() const override;
    QIcon icon(Dock::IconType, Dock::ThemeType) const override;

private slots:
    void onRestReminder(qint64 workedMs);
    void onContinuousWorkChanged(qint64 workedMs);

private:
    QString notificationIconPath();
    void applyRestInterval(int minutes, bool persist);
    int restIntervalMinutes() const;
    void reloadRestIntervalIfChanged();
    qint64 settingsMtime() const;

    QTimer *m_settingsTimer = nullptr;
    qint64 m_lastSettingsMtime = -1;

    QString m_itemKey;
    QLabel *m_itemLabel = nullptr;
    UsagePopupWidget *m_popup = nullptr;
    UsageTracker *m_tracker = nullptr;
    ContextSwitchMonitor *m_switchMonitor = nullptr;
    UsageStorage *m_storage = nullptr;
    RestNotifier *m_notifier = nullptr;
    QLabel *m_tips = nullptr;
    QString m_notifyIcon;
};

#endif // APPUSAGEMONITORPLUGIN_H
