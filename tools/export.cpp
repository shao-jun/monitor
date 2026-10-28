// app-usage-monitor-export：把数据库里的数据导成 CSV，方便拿去做表/复盘
//
// 用法：
//   app-usage-monitor-export                      # 导出最近 14 天到 ~/下载 目录
//   app-usage-monitor-export --from 2026-10-01 --to 2026-10-14
//   app-usage-monitor-export --days 30 --out /tmp/dump
//   app-usage-monitor-export --json               # 另外再存一份 json
//
// 产出（UTF-8，带 BOM，Excel/WPS 直接打开不乱码）：
//   daily_usage.csv   每天每个应用的 操作/查看 分钟数
//   hourly_usage.csv  每天每小时的 操作/查看/锁屏 分钟数
//   locks.csv         每天的锁屏分钟数
//   switches.csv      每次上下文切换的明细
//   events.csv        手动事件明细
//   summary.csv       每天一行：切换次数、碎片切换、外部事件次数与分钟数

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVector>
#include <QHash>
#include <QPair>
#include <QStandardPaths>
#include <QTextStream>
#include <QStringList>

#include <cstdio>

namespace {

QString g_dbPath;
QStringList g_written;

double minutes(qint64 ms)
{
    return double(ms) / 60000.0;
}

QString fixed1(double v)
{
    return QString::number(v, 'f', 1);
}

// CSV 字段引用：有逗号/引号/换行就加引号
QString csvField(const QString &text)
{
    QString s = text;
    s.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n')))
        return QLatin1Char('"') + s + QLatin1Char('"');
    return s;
}

QString csvLine(const QStringList &fields)
{
    QStringList out;
    out.reserve(fields.size());
    for (const QString &f : fields)
        out.append(csvField(f));
    return out.join(QLatin1Char(',')) + QLatin1Char('\n');
}

bool openCsv(const QString &dir, const QString &name, QFile &file, QTextStream &ts)
{
    const QString path = QDir(dir).filePath(name);
    file.setFileName(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "写不了 %s\n", path.toUtf8().constData());
        return false;
    }
    ts.setDevice(&file);
    ts.setCodec("UTF-8");
    ts.setGenerateByteOrderMark(true); // 让 Excel / WPS 直接认出 UTF-8
    g_written.append(path);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("app-usage-monitor-export"));

    const QDate today = QDate::currentDate();
    QDate from = today.addDays(-13);
    QDate to = today;
    QString outDir;
    bool wantJson = false;

    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        const QString &a = args.at(i);
        auto next = [&](const QString &fallback) {
            return (i + 1 < args.size()) ? args.at(++i) : fallback;
        };
        if (a == QLatin1String("--from")) {
            const QDate d = QDate::fromString(next(QString()), QStringLiteral("yyyy-MM-dd"));
            if (d.isValid())
                from = d;
        } else if (a == QLatin1String("--to")) {
            const QDate d = QDate::fromString(next(QString()), QStringLiteral("yyyy-MM-dd"));
            if (d.isValid())
                to = d;
        } else if (a == QLatin1String("--days")) {
            bool ok = false;
            const int n = next(QStringLiteral("14")).toInt(&ok);
            if (ok && n > 0) {
                from = today.addDays(-(n - 1));
                to = today;
            }
        } else if (a == QLatin1String("--out")) {
            outDir = next(QString());
        } else if (a == QLatin1String("--db")) {
            g_dbPath = next(QString());
        } else if (a == QLatin1String("--json")) {
            wantJson = true;
        } else if (a == QLatin1String("-h") || a == QLatin1String("--help")) {
            std::printf("用法：app-usage-monitor-export [--from YYYY-MM-DD] [--to YYYY-MM-DD] "
                        "[--days N] [--out DIR] [--db PATH] [--json]\n");
            return 0;
        }
    }

    if (to < from) {
        std::fprintf(stderr, "--to 比 --from 还早，改一下时间范围\n");
        return 2;
    }

    if (g_dbPath.isEmpty()) {
        g_dbPath = QDir::home().filePath(QStringLiteral(".local/share/app-usage-monitor/usage.db"));
    }
    if (!QFileInfo::exists(g_dbPath)) {
        std::fprintf(stderr, "找不到数据库：%s\n", g_dbPath.toUtf8().constData());
        return 1;
    }

    if (outDir.isEmpty()) {
        QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (base.isEmpty())
            base = QDir::homePath();
        outDir = QDir(base).filePath(QStringLiteral("app-usage-export-%1_%2")
                                         .arg(from.toString(QStringLiteral("yyyyMMdd")),
                                              to.toString(QStringLiteral("yyyyMMdd"))));
    }
    if (!QDir().mkpath(outDir)) {
        std::fprintf(stderr, "创建目录失败：%s\n", outDir.toUtf8().constData());
        return 1;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("export"));
    db.setDatabaseName(g_dbPath);
    if (!db.open()) {
        std::fprintf(stderr, "打不开数据库：%s\n", db.lastError().text().toUtf8().constData());
        return 1;
    }

    const QString fromStr = from.toString(QStringLiteral("yyyy-MM-dd"));
    const QString toStr = to.toString(QStringLiteral("yyyy-MM-dd"));
    const qint64 fromTs = QDateTime(from, QTime(0, 0)).toSecsSinceEpoch();
    const qint64 toTs = QDateTime(to, QTime(23, 59, 59)).toSecsSinceEpoch();

    // ---- daily_usage.csv ----
    {
        QFile f;
        QTextStream ts;
        if (!openCsv(outDir, QStringLiteral("daily_usage.csv"), f, ts))
            return 1;
        ts << csvLine({QStringLiteral("date"), QStringLiteral("app"), QStringLiteral("display_name"),
                       QStringLiteral("active_minutes"), QStringLiteral("viewing_minutes"),
                       QStringLiteral("total_minutes")});
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT date, app, display_name, active_ms, viewing_ms FROM usage "
                                 "WHERE date BETWEEN :from AND :to ORDER BY date ASC, (active_ms + viewing_ms) DESC"));
        q.bindValue(QStringLiteral(":from"), fromStr);
        q.bindValue(QStringLiteral(":to"), toStr);
        q.exec();
        while (q.next()) {
            const qint64 a = q.value(3).toLongLong();
            const qint64 v = q.value(4).toLongLong();
            ts << csvLine({q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                           fixed1(minutes(a)), fixed1(minutes(v)), fixed1(minutes(a + v))});
        }
    }

    // ---- hourly_usage.csv ----
    {
        QFile f;
        QTextStream ts;
        if (!openCsv(outDir, QStringLiteral("hourly_usage.csv"), f, ts))
            return 1;
        ts << csvLine({QStringLiteral("date"), QStringLiteral("hour"), QStringLiteral("active_minutes"),
                       QStringLiteral("viewing_minutes"), QStringLiteral("lock_minutes")});
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT date, hour, active_ms, viewing_ms, lock_ms FROM hourly_usage "
                                 "WHERE date BETWEEN :from AND :to ORDER BY date ASC, hour ASC"));
        q.bindValue(QStringLiteral(":from"), fromStr);
        q.bindValue(QStringLiteral(":to"), toStr);
        q.exec();
        while (q.next()) {
            ts << csvLine({q.value(0).toString(), QString::number(q.value(1).toInt()),
                           fixed1(minutes(q.value(2).toLongLong())), fixed1(minutes(q.value(3).toLongLong())),
                           fixed1(minutes(q.value(4).toLongLong()))});
        }
    }

    // ---- locks.csv ----
    {
        QFile f;
        QTextStream ts;
        if (!openCsv(outDir, QStringLiteral("locks.csv"), f, ts))
            return 1;
        ts << csvLine({QStringLiteral("date"), QStringLiteral("lock_minutes")});
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT date, lock_ms FROM locks WHERE date BETWEEN :from AND :to ORDER BY date ASC"));
        q.bindValue(QStringLiteral(":from"), fromStr);
        q.bindValue(QStringLiteral(":to"), toStr);
        q.exec();
        while (q.next())
            ts << csvLine({q.value(0).toString(), fixed1(minutes(q.value(1).toLongLong()))});
    }

    // ---- switches.csv ----
    int switchCount = 0;
    {
        QFile f;
        QTextStream ts;
        if (!openCsv(outDir, QStringLiteral("switches.csv"), f, ts))
            return 1;
        ts << csvLine({QStringLiteral("datetime"), QStringLiteral("kind"), QStringLiteral("from_app"),
                       QStringLiteral("to_app"), QStringLiteral("from_context"),
                       QStringLiteral("to_context"), QStringLiteral("dwell_seconds")});
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT ts, kind, from_app, to_app, from_ctx, to_ctx, dwell_sec FROM switches "
                                 "WHERE ts BETWEEN :from AND :to ORDER BY ts ASC"));
        q.bindValue(QStringLiteral(":from"), fromTs);
        q.bindValue(QStringLiteral(":to"), toTs);
        q.exec();
        while (q.next()) {
            const QDateTime when = QDateTime::fromSecsSinceEpoch(q.value(0).toLongLong());
            ts << csvLine({when.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), q.value(1).toString(),
                           q.value(2).toString(), q.value(3).toString(), q.value(4).toString(),
                           q.value(5).toString(), QString::number(q.value(6).toInt())});
            ++switchCount;
        }
    }

    // ---- events.csv ----
    int eventCount = 0;
    {
        QFile f;
        QTextStream ts;
        if (!openCsv(outDir, QStringLiteral("events.csv"), f, ts))
            return 1;
        ts << csvLine({QStringLiteral("date"), QStringLiteral("start"), QStringLiteral("end"),
                       QStringLiteral("minutes"), QStringLiteral("label")});
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT start_ts, end_ts, label FROM events "
                                 "WHERE date BETWEEN :from AND :to ORDER BY start_ts ASC"));
        q.bindValue(QStringLiteral(":from"), fromStr);
        q.bindValue(QStringLiteral(":to"), toStr);
        q.exec();
        while (q.next()) {
            const QDateTime s = QDateTime::fromSecsSinceEpoch(q.value(0).toLongLong());
            const QDateTime e = QDateTime::fromSecsSinceEpoch(q.value(1).toLongLong());
            ts << csvLine({s.toString(QStringLiteral("yyyy-MM-dd")), s.toString(QStringLiteral("HH:mm")),
                           e.toString(QStringLiteral("HH:mm")),
                           fixed1(double(q.value(1).toLongLong() - q.value(0).toLongLong()) / 60.0),
                           q.value(2).toString()});
            ++eventCount;
        }
    }

    // ---- summary.csv：每天一行 ----
    {
        QFile f;
        QTextStream ts;
        if (!openCsv(outDir, QStringLiteral("summary.csv"), f, ts))
            return 1;
        ts << csvLine({QStringLiteral("date"), QStringLiteral("switches"), QStringLiteral("fragmented"),
                       QStringLiteral("longest_focus_minutes"), QStringLiteral("events"),
                       QStringLiteral("event_minutes")});

        QHash<QString, QVector<qint64>> dwellByDay;
        QSqlQuery sq(db);
        sq.prepare(QStringLiteral("SELECT date, dwell_sec FROM switches WHERE date BETWEEN :from AND :to"));
        sq.bindValue(QStringLiteral(":from"), fromStr);
        sq.bindValue(QStringLiteral(":to"), toStr);
        sq.exec();
        while (sq.next())
            dwellByDay[sq.value(0).toString()].append(sq.value(1).toLongLong());

        QHash<QString, QPair<int, qint64>> eventByDay;
        QSqlQuery eq(db);
        eq.prepare(QStringLiteral("SELECT date, start_ts, end_ts FROM events WHERE date BETWEEN :from AND :to"));
        eq.bindValue(QStringLiteral(":from"), fromStr);
        eq.bindValue(QStringLiteral(":to"), toStr);
        eq.exec();
        while (eq.next()) {
            QPair<int, qint64> &acc = eventByDay[eq.value(0).toString()];
            acc.first += 1;
            acc.second += qMax<qint64>(0, eq.value(2).toLongLong() - eq.value(1).toLongLong());
        }

        for (QDate d = from; d <= to; d = d.addDays(1)) {
            const QString key = d.toString(QStringLiteral("yyyy-MM-dd"));
            const QVector<qint64> dwells = dwellByDay.value(key);
            int fragmented = 0;
            qint64 longest = 0;
            for (qint64 v : dwells) {
                if (v < 10)
                    ++fragmented;
                longest = qMax(longest, v);
            }
            const QPair<int, qint64> ev = eventByDay.value(key);
            ts << csvLine({key, QString::number(dwells.size()), QString::number(fragmented),
                           longest > 0 ? fixed1(double(longest) / 60.0) : QStringLiteral("0.0"),
                           QString::number(ev.first), fixed1(double(ev.second) / 60.0)});
        }
    }

    if (wantJson) {
        QFile f;
        QTextStream ts;
        if (!openCsv(outDir, QStringLiteral("export.json"), f, ts)) // BOM 对 json 无害
            return 1;
        ts << QStringLiteral("{\n");
        ts << QStringLiteral("  \"from\": \"%1\",\n  \"to\": \"%2\",\n").arg(fromStr, toStr);
        ts << QStringLiteral("  \"switches\": %1,\n  \"events\": %2,\n").arg(switchCount).arg(eventCount);
        ts << QStringLiteral("  \"database\": \"%1\"\n}\n").arg(g_dbPath);
    }

    std::printf("已导出 %s ~ %s\n", fromStr.toUtf8().constData(), toStr.toUtf8().constData());
    std::printf("目录：%s\n", outDir.toUtf8().constData());
    for (const QString &path : g_written)
        std::printf("  %s\n", QFileInfo(path).fileName().toUtf8().constData());
    std::printf("切换 %d 条，事件 %d 条\n", switchCount, eventCount);
    return 0;
}
