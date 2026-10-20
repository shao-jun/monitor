#include "appregistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QTextStream>

AppRegistry::AppRegistry() {}

static QString localizedName(const QMap<QString, QString> &names)
{
    QStringList langs;
    const QString full = QLocale::system().name(); // zh_CN
    langs << full;
    const QString lang = full.section('_', 0, 0);
    if (!lang.isEmpty() && lang != full)
        langs << lang;
    langs << QStringLiteral("en_US") << QStringLiteral("en") << QStringLiteral("C");

    for (const QString &l : langs) {
        auto it = names.constFind(l);
        if (it != names.constEnd())
            return it.value();
    }
    auto def = names.constFind(QString());
    if (def != names.constEnd())
        return def.value();
    return names.isEmpty() ? QString() : names.constBegin().value();
}

void AppRegistry::ensureLoaded() const
{
    if (m_loaded)
        return;
    m_loaded = true;

    const QByteArray xdgData = qgetenv("XDG_DATA_HOME");
    QStringList dirs;
    dirs << (xdgData.isEmpty()
                 ? QDir::home().filePath(QStringLiteral(".local/share/applications"))
                 : QString::fromLocal8Bit(xdgData) + QStringLiteral("/applications"));
    dirs << QStringLiteral("/usr/share/applications");
    dirs << QStringLiteral("/usr/local/share/applications");

    for (const QString &dir : dirs) {
        const QDir d(dir);
        if (!d.exists())
            continue;
        const QStringList files = d.entryList(QStringList() << QStringLiteral("*.desktop"), QDir::Files);
        for (const QString &f : files) {
            QFile file(d.filePath(f));
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;

            QMap<QString, QString> names;
            QString icon;
            QString exec;
            QString wmclass;
            bool inDesktop = false;

            QTextStream ts(&file);
            ts.setCodec("UTF-8");
            while (!ts.atEnd()) {
                const QString line = ts.readLine().trimmed();
                if (line.isEmpty() || line.startsWith('#'))
                    continue;
                if (line.startsWith('[')) {
                    inDesktop = (line == QLatin1String("[Desktop Entry]"));
                    continue;
                }
                if (!inDesktop)
                    continue;
                const int eq = line.indexOf('=');
                if (eq < 0)
                    continue;
                const QString key = line.left(eq).trimmed();
                const QString val = line.mid(eq + 1).trimmed();
                if (key == QLatin1String("Name"))
                    names[QString()] = val;
                else if (key.startsWith(QLatin1String("Name["))) {
                    // Name[zh_CN] / Name[zh_CN.UTF-8]
                    QString loc = key.mid(5, key.length() - 6);
                    if (loc.contains('.'))
                        loc = loc.section('.', 0, 0);
                    names[loc] = val;
                } else if (key == QLatin1String("Icon"))
                    icon = val;
                else if (key == QLatin1String("Exec"))
                    exec = val;
                else if (key == QLatin1String("StartupWMClass"))
                    wmclass = val;
            }
            file.close();

            AppMeta meta;
            meta.name = localizedName(names);
            meta.icon = icon;
            if (meta.name.isEmpty())
                continue;

            const QString desktopId = QFileInfo(file.fileName()).completeBaseName();

            if (!exec.isEmpty()) {
                QString e = exec;
                e.remove(QRegularExpression(QStringLiteral("[\"']")));
                const QString first = e.section(' ', 0, 0);
                const QString base = QFileInfo(first).completeBaseName();
                if (!base.isEmpty())
                    m_byExec[base.toLower()] = meta;
            }
            if (!wmclass.isEmpty())
                m_byWmClass[wmclass.toLower()] = meta;
            if (!desktopId.isEmpty())
                m_byDesktopId[desktopId.toLower()] = meta;
        }
    }
}

AppMeta AppRegistry::resolve(const QString &wmClassInstance,
                             const QString &wmClass,
                             const QString &processName) const
{
    ensureLoaded();

    const QString ins = wmClassInstance.toLower();
    const QString cls = wmClass.toLower();
    const QString proc = processName.toLower();

    if (!ins.isEmpty() && m_byWmClass.contains(ins))
        return m_byWmClass.value(ins);
    if (!cls.isEmpty() && m_byWmClass.contains(cls))
        return m_byWmClass.value(cls);
    if (!proc.isEmpty() && m_byExec.contains(proc))
        return m_byExec.value(proc);
    if (!proc.isEmpty() && m_byDesktopId.contains(proc))
        return m_byDesktopId.value(proc);
    if (!ins.isEmpty() && m_byDesktopId.contains(ins))
        return m_byDesktopId.value(ins);

    for (auto it = m_byExec.constBegin(); it != m_byExec.constEnd(); ++it) {
        if (!proc.isEmpty() && (proc.contains(it.key()) || it.key().contains(proc)))
            return it.value();
    }

    return fallbackFor(wmClassInstance.isEmpty() ? processName : wmClassInstance);
}

AppMeta AppRegistry::fallbackFor(const QString &hint)
{
    AppMeta m;
    QString base = hint;
    if (base.isEmpty())
        base = QStringLiteral("未知应用");
    base = base.section('/', -1);
    if (base.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive))
        base.chop(4);
    if (base.endsWith(QLatin1String(".desktop"), Qt::CaseInsensitive))
        base.chop(8);
    m.name = base;
    m.icon.clear();
    return m;
}
