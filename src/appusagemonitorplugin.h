#ifndef APPUSAGEMONITORPLUGIN_H
#define APPUSAGEMONITORPLUGIN_H

#include "pluginsiteminterface_v2.h"

#include <QObject>

class UsageTracker;
class UsagePopupWidget;
class UsageStorage;
class QLabel;
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

    Dock::PluginFlags flags() const override;
    QIcon icon(Dock::IconType, Dock::ThemeType) const override;

private slots:
    void onRestReminder(qint64 workedMs);
    void onContinuousWorkChanged(qint64 workedMs);

private:
    QString notificationIconPath();

    QString m_itemKey;
    QLabel *m_itemLabel = nullptr;
    UsagePopupWidget *m_popup = nullptr;
    UsageTracker *m_tracker = nullptr;
    UsageStorage *m_storage = nullptr;
    RestNotifier *m_notifier = nullptr;
    QLabel *m_tips = nullptr;
    QString m_notifyIcon;
};

#endif // APPUSAGEMONITORPLUGIN_H
