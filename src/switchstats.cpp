#include "switchstats.h"

#include <QDateTime>

#include <algorithm>
#include <QHash>

namespace {

bool isWxWork(const QString &app)
{
    return app.contains(QStringLiteral("企业微信")) || app.contains(QStringLiteral("wxwork"), Qt::CaseInsensitive);
}

// 企业微信那一侧不写「企业微信」，写具体聊的是谁：私聊是人名，群聊是群名
QString sideLabel(const QString &app, const QString &ctx)
{
    if (!isWxWork(app) || ctx.isEmpty() || ctx == app || isWxWork(ctx))
        return app;
    return ctx;
}

QString elide(const QString &text, int max = 24)
{
    return text.size() > max ? text.left(max - 1) + QChar(0x2026) : text;
}

} // namespace

SwitchStats computeSwitchStats(const QVector<SwitchRow> &rows)
{
    SwitchStats st;
    st.perHour = QVector<int>(24, 0);
    if (rows.isEmpty())
        return st;

    QHash<QString, int> transitions;
    QHash<QString, int> apps;
    QHash<QString, int> chats;

    for (const SwitchRow &r : rows) {
        ++st.total;
        if (r.dwellSec >= 0 && r.dwellSec < 10)
            ++st.fragmented;

        const QDateTime when = QDateTime::fromSecsSinceEpoch(r.ts);
        const int hour = when.time().hour();
        if (hour >= 0 && hour < 24)
            st.perHour[hour] += 1;

        if (st.firstTs == 0 || r.ts < st.firstTs)
            st.firstTs = r.ts;
        if (r.ts > st.lastTs)
            st.lastTs = r.ts;

        // 企业微信内部换会话也算一次跳转（「荷浩 → 项目群」），所以要按具体会话拼标签，
        // 不能再用 fromApp != toApp 过滤
        if (!r.toApp.isEmpty())
            ++apps[r.toApp];

        const QString fromSide = elide(sideLabel(r.fromApp, r.fromCtx));
        const QString toSide = elide(sideLabel(r.toApp, r.toCtx));
        if (!fromSide.isEmpty() && !toSide.isEmpty() && fromSide != toSide)
            ++transitions[QStringLiteral("%1 → %2").arg(fromSide, toSide)];

        if (r.kind == QLatin1String("chat")) {
            if (!r.toCtx.isEmpty())
                ++chats[r.toCtx];
        }
    }

    for (int h = 0; h < 24; ++h) {
        if (st.perHour[h] > st.peakCount) {
            st.peakCount = st.perHour[h];
            st.peakHour = h;
        }
    }

    auto top = [](const QHash<QString, int> &src, int limit) {
        QVector<QPair<QString, int>> list;
        for (auto it = src.constBegin(); it != src.constEnd(); ++it)
            list.append(qMakePair(it.key(), it.value()));
        std::sort(list.begin(), list.end(), [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
            return a.second > b.second;
        });
        if (list.size() > limit)
            list.resize(limit);
        return list;
    };

    st.topApps = top(apps, 6);
    st.topTransitions = top(transitions, 5);
    st.topChats = top(chats, 5);

    if (st.total > 1 && st.lastTs > st.firstTs) {
        const double minutes = double(st.lastTs - st.firstTs) / 60.0;
        st.avgIntervalMin = minutes / double(st.total);
    }
    return st;
}
