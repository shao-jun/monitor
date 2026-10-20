#ifndef APPREGISTRY_H
#define APPREGISTRY_H

#include <QMap>
#include <QString>

struct AppMeta {
    QString name; // 展示名
    QString icon; // 图标名或图标文件路径
};

// 扫描 .desktop 文件，把进程名 / WM_CLASS 解析成友好的应用名与图标
class AppRegistry
{
public:
    AppRegistry();

    AppMeta resolve(const QString &wmClassInstance,
                    const QString &wmClass,
                    const QString &processName) const;

    static AppMeta fallbackFor(const QString &hint);

private:
    void ensureLoaded() const;

    mutable bool m_loaded = false;
    mutable QMap<QString, AppMeta> m_byWmClass;
    mutable QMap<QString, AppMeta> m_byExec;
    mutable QMap<QString, AppMeta> m_byDesktopId;
};

#endif // APPREGISTRY_H
