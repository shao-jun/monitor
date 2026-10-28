#include "switchstats.h"

#include <QDateTime>

#include <algorithm>
#include <QHash>

SwitchStats computeSwitchStats(const QVector<SwitchRow> &rows)
{
    SwitchStats st;
    st.perHour = QVector<int>(24, 0);
    if (rows.isEmpty())
        return st;

    QHash<QString, int> transitions;
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

        if (!r.fromApp.isEmpty() && !r.toApp.isEmpty() && r.fromApp != r.toApp)
            ++transitions[QStringLiteral("%1 → %2").arg(r.fromApp, r.toApp)];

        if (r.kind == QLatin1String("chat") && !r.toCtx.isEmpty())
            ++chats[r.toCtx];
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

    st.topTransitions = top(transitions, 5);
    st.topChats = top(chats, 5);

    if (st.total > 1 && st.lastTs > st.firstTs) {
        const double minutes = double(st.lastTs - st.firstTs) / 60.0;
        st.avgIntervalMin = minutes / double(st.total);
    }
    return st;
}
