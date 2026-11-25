#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""app-usage-monitor-report — 把 usage.db 里的数据画成一张本地网页报告

用法：
    app-usage-monitor-report                 # 最近 30 天，写到 ~/下载/app-usage-switch-report.html
    app-usage-monitor-report --days 14
    app-usage-monitor-report --from 2026-10-01 --to 2026-10-31 --out /tmp/report.html

只读本地数据库（默认 ~/.local/share/app-usage-monitor/usage.db），生成的 HTML
自带全部样式与图表（内联 SVG），不加载任何外部资源，断网也能双击打开。
"""

import argparse
import datetime as dt
import html
import os
import sqlite3
import sys
from collections import defaultdict

DB_DEFAULT = os.path.expanduser("~/.local/share/app-usage-monitor/usage.db")
WXWORK_NAMES = ("企业微信", "wxwork")

# 统计口径：原始数据里混着不少「假切换」，报告按下面的规则算一遍干净的
MIN_DWELL_SEC = 3        # 上一屏待不到 3 秒就切走，是焦点抖动/OCR 抖，不算一次切换
SESSION_GAP_SEC = 1200   # 间隔 20 分钟以上算离开后回来（午休、长会），不接着算
SYSTEM_APPS = ("dde-dock", "深度控制中心", "dde-select-dialog", "剪贴板", "dde-osd",
               "Dde-select-dialog", "dde-session-ui", "深度文件管理器预览")
FOCUS_MIN_SEC = 900      # 连续待在一个上下文 ≥15 分钟算一个专注块
FOCUS_MAX_SEC = 3600     # 超过 1 小时的不算（那是走开了/开会去了）

WEEKDAY_CN = ("周一", "周二", "周三", "周四", "周五", "周六", "周日")

# 停留时长分桶（秒）
DWELL_BUCKETS = [(0, 3, "<3 秒"), (3, 10, "3–10 秒"), (10, 30, "10–30 秒"),
                 (30, 60, "30 秒–1 分"), (60, 300, "1–5 分"), (300, 900, "5–15 分"),
                 (900, 3600, "15–60 分"), (3600, 10 ** 9, ">1 小时")]


# ---------------------------------------------------------------- 小工具

def esc(s):
    return html.escape(str(s), quote=True)


def fmt_min(minutes, short=False):
    m = int(round(minutes))
    if short:
        if m < 60:
            return "%d分" % m
        return "%d小时%d分" % (m // 60, m % 60) if m % 60 else "%d小时" % (m // 60)
    if m < 60:
        return "%d 分" % m
    return "%d 小时 %d 分" % (m // 60, m % 60) if m % 60 else "%d 小时" % (m // 60)


def hour_label(h):
    return "%02d:00" % h


def median(xs):
    if not xs:
        return 0
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0 or syy <= 0:
        return None
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    return sxy / (sxx ** 0.5 * syy ** 0.5)


# ---------------------------------------------------------------- 读库

def load(db_path, start, end):
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    con.row_factory = sqlite3.Row

    days = []
    d = start
    while d <= end:
        days.append(d)
        d += dt.timedelta(days=1)

    per_day = {}
    for d in days:
        per_day[d.isoformat()] = {
            "date": d, "total": 0, "raw": 0, "inner": 0, "frag": 0, "hours": [0] * 24,
            "dwells": [], "chats": set(), "apps": defaultdict(int),
            "active_ms": 0, "viewing_ms": 0, "lock_ms": 0,
            "focus_blocks": 0, "focus_sec": 0, "longest_focus": 0,
            "events": 0, "event_min": 0,
            "hour_switches": [0] * 24, "hour_on": [0.0] * 24,
            "sw_per_hour": [0] * 24,
        }

    start_ts = int(dt.datetime.combine(start, dt.time(0, 0)).timestamp())
    end_ts = int(dt.datetime.combine(end, dt.time(23, 59, 59)).timestamp())

    app_enter = defaultdict(int)      # 被切进去的次数
    app_leave = defaultdict(int)      # 从这个应用切走的次数
    app_leave_sec = defaultdict(int)  # 在它里面待过的时间（切走时的停留时长累加）
    pair_count = defaultdict(int)     # from_app → to_app
    hist = [0] * len(DWELL_BUCKETS)
    chat_count = defaultdict(int)
    chat_day = defaultdict(set)
    scatter = []                      # (当天该小时的操作分钟, 该小时切换数)
    sw_total = [0] * 24
    sw_days = [0] * 24

    rows = con.execute(
        "SELECT ts, kind, from_app, to_app, from_ctx, to_ctx, dwell_sec "
        "FROM switches WHERE ts BETWEEN ? AND ? ORDER BY ts", (start_ts, end_ts))
    for r in rows:
        when = dt.datetime.fromtimestamp(r["ts"])
        key = when.date().isoformat()
        rec = per_day.get(key)
        if rec is None:
            continue
        rec["raw"] += 1
        from_app = r["from_app"] or ""
        to_app = r["to_app"] or ""
        dwell = r["dwell_sec"] or 0

        # 应用内换窗口/换页（Chrome 换标签、资源管理器换目录、企业微信换会话之外的同应用动作）
        # 不是「上下文切换」，单列计数但不进有效切换，否则切换数虚高一倍。
        if r["kind"] == "inner" or (from_app and from_app == to_app):
            rec["inner"] += 1
            continue

        # 停留时长分布和「在某个应用里待多久」用原始 dwell（不含系统弹层）
        if from_app and not any(a in from_app for a in SYSTEM_APPS):
            for i, (lo, hi, _) in enumerate(DWELL_BUCKETS):
                if lo <= dwell < hi:
                    hist[i] += 1
                    break
            app_leave[from_app] += 1
            app_leave_sec[from_app] += dwell

        if any(a in to_app or a in from_app for a in SYSTEM_APPS):
            continue
        if dwell < MIN_DWELL_SEC or dwell >= SESSION_GAP_SEC:
            continue

        rec["total"] += 1
        rec["dwells"].append(dwell)
        if dwell < 10:
            rec["frag"] += 1
        rec["hours"][when.hour] += 1
        rec["sw_per_hour"][when.hour] += 1
        if to_app:
            app_enter[to_app] += 1
            rec["apps"][to_app] += 1
        if from_app and to_app:
            pair_count[(from_app, to_app)] += 1

        # 专注块：在某个上下文连续待满 15 分钟
        if FOCUS_MIN_SEC <= dwell < FOCUS_MAX_SEC:
            rec["focus_blocks"] += 1
            rec["focus_sec"] += dwell
            rec["longest_focus"] = max(rec["longest_focus"], dwell)

        ctx = r["to_ctx"] or ""
        if any(w in to_app for w in WXWORK_NAMES) and ctx and ctx not in WXWORK_NAMES \
                and not any(w in ctx for w in WXWORK_NAMES):
            rec["chats"].add(ctx)
            chat_count[ctx] += 1
            chat_day[ctx].add(key)

    # 每小时用量 + 锁屏
    for r in con.execute(
            "SELECT date, hour, active_ms, viewing_ms, lock_ms FROM hourly_usage "
            "WHERE date BETWEEN ? AND ?", (start.isoformat(), end.isoformat())):
        rec = per_day.get(r["date"])
        if not rec:
            continue
        a = r["active_ms"] or 0
        v = r["viewing_ms"] or 0
        rec["active_ms"] += a
        rec["viewing_ms"] += v
        rec["lock_ms"] += min(r["lock_ms"] or 0, 3600000)
        rec["hour_on"][r["hour"]] = (a + v) / 60000.0
        if a + v > 0:
            sw_total[r["hour"]] += rec["sw_per_hour"][r["hour"]]
            sw_days[r["hour"]] += 1
            scatter.append(((a + v) / 60000.0, rec["sw_per_hour"][r["hour"]], r["date"], r["hour"]))

    # 应用时长（usage 表按应用按天累计）
    app_ms = defaultdict(int)
    app_name = {}
    for r in con.execute(
            "SELECT app, display_name, SUM(active_ms + viewing_ms) ms FROM usage "
            "WHERE date BETWEEN ? AND ? GROUP BY app", (start.isoformat(), end.isoformat())):
        name = (r["display_name"] or r["app"] or "?").strip() or r["app"]
        app_name[r["app"]] = name
        app_ms[name] += r["ms"] or 0

    # 在岗时长以 usage 表为准（同一区间它覆盖的天数更全，跟应用明细同源）
    usage_on = defaultdict(float)
    usage_active = defaultdict(float)
    for r in con.execute(
            "SELECT date, SUM(active_ms) a, SUM(viewing_ms) v FROM usage "
            "WHERE date BETWEEN ? AND ? GROUP BY date", (start.isoformat(), end.isoformat())):
        usage_on[r["date"]] = ((r["a"] or 0) + (r["v"] or 0)) / 60000.0
        usage_active[r["date"]] = (r["a"] or 0) / 60000.0

    # 事件
    events = []
    for r in con.execute(
            "SELECT date, start_ts, end_ts, label FROM events WHERE date BETWEEN ? AND ? "
            "ORDER BY start_ts", (start.isoformat(), end.isoformat())):
        minutes = max(0, (r["end_ts"] or 0) - (r["start_ts"] or 0)) // 60
        when = dt.datetime.fromtimestamp(r["start_ts"])
        events.append({"date": r["date"], "when": when, "minutes": minutes,
                       "label": (r["label"] or "").strip() or "（无说明）"})
        rec = per_day.get(r["date"])
        if rec:
            rec["events"] += 1
            rec["event_min"] += minutes

    con.close()

    out_days = []
    for d in days:
        rec = per_day[d.isoformat()]
        out_days.append({
            "date": d,
            "total": rec["total"],
            "raw": rec["raw"],
            "frag": rec["frag"],
            "median_dwell": median(rec["dwells"]),
            "chats": len(rec["chats"]),
            "apps": dict(rec["apps"]),
            "hours": rec["hours"],
            "hour_on": rec["hour_on"],
            "hour_switches": rec["sw_per_hour"],
            "on_min": usage_on.get(d.isoformat(), (rec["active_ms"] + rec["viewing_ms"]) / 60000.0),
            "active_min": usage_active.get(d.isoformat(), rec["active_ms"] / 60000.0),
            "viewing_min": max(0.0, usage_on.get(d.isoformat(),
                                                 (rec["active_ms"] + rec["viewing_ms"]) / 60000.0)
                               - usage_active.get(d.isoformat(), rec["active_ms"] / 60000.0)),
            "lock_min": rec["lock_ms"] / 60000.0,
            "focus_blocks": rec["focus_blocks"],
            "focus_min": rec["focus_sec"] / 60.0,
            "longest_focus": rec["longest_focus"] / 60.0,
            "events": rec["events"],
            "event_min": rec["event_min"],
        })

    # 把 switches 里的进程名映射成 usage 表里的显示名
    def nice(app):
        if not app:
            return "?"
        if app in app_name:
            return app_name[app]
        low = app.lower()
        for k, v in app_name.items():
            if k.lower() == low:
                return v
        for w in WXWORK_NAMES:
            if w in app:
                return "企业微信"
        return app[:1].upper() + app[1:]

    enter = defaultdict(int)
    for k, v in app_enter.items():
        enter[nice(k)] += v
    leave = defaultdict(int)
    leave_sec = defaultdict(int)
    for k, v in app_leave.items():
        leave[nice(k)] += v
        leave_sec[nice(k)] += app_leave_sec[k]
    pairs = defaultdict(int)
    for (a, b), v in pair_count.items():
        pairs[(nice(a), nice(b))] += v

    return {
        "days": out_days,
        "apps": dict(app_ms),
        "enter": dict(enter),
        "leave": dict(leave),
        "leave_sec": dict(leave_sec),
        "pairs": dict(pairs),
        "hist": hist,
        "chats": dict(chat_count),
        "chat_days": {k: len(v) for k, v in chat_day.items()},
        "events": events,
        "scatter": scatter,
        "hourly_switch_avg": [(sw_total[h] / sw_days[h]) if sw_days[h] else 0.0
                              for h in range(24)],
    }


# ---------------------------------------------------------------- 汇总

def moving_average(values, window=7):
    out = []
    for i in range(len(values)):
        lo = max(0, i - window + 1)
        chunk = values[lo:i + 1]
        out.append(sum(chunk) / len(chunk))
    return out


def summarize(data):
    days = data["days"]
    active = [d for d in days if d["total"] > 0 or d["on_min"] > 0]
    sw_days = [d for d in days if d["total"] > 0]
    on_days = [d for d in days if d["on_min"] > 0]
    lock_days = [d for d in days if d["lock_min"] > 0]
    total = sum(d["total"] for d in days)
    frag = sum(d["frag"] for d in days)
    s = {
        "days": len(days),
        "active_days": len(active),
        "sw_days": len(sw_days),
        "on_days": len(on_days),
        "lock_days": len(lock_days),
        "total": total,
        "raw": sum(d["raw"] for d in days),
        "avg": total / len(sw_days) if sw_days else 0,
        "frag": frag,
        "frag_pct": (frag / total * 100.0) if total else 0,
        "on_min": sum(d["on_min"] for d in days),
        "active_min": sum(d["active_min"] for d in days),
        "viewing_min": sum(d["viewing_min"] for d in days),
        "lock_min": sum(d["lock_min"] for d in days),
        "on_avg": (sum(d["on_min"] for d in on_days) / len(on_days)) if on_days else 0,
        "focus_blocks": sum(d["focus_blocks"] for d in days),
        "focus_min": sum(d["focus_min"] for d in days),
        "longest_focus": max([d["longest_focus"] for d in days] + [0]),
        "chats": max([d["chats"] for d in days] + [0]),
        "events": sum(d["events"] for d in days),
        "event_min": sum(d["event_min"] for d in days),
        "busiest": max(active, key=lambda d: d["total"]) if active else None,
        "calmest": min(active, key=lambda d: d["total"]) if active else None,
        "median_dwell": median([d["median_dwell"] for d in days if d["total"] > 0]),
    }
    s["focus_avg"] = s["focus_blocks"] / len(sw_days) if sw_days else 0

    tail = [d for d in days[-7:] if d["total"] > 0]
    prev = [d for d in days[-14:-7] if d["total"] > 0]
    s["trend"] = None
    if len(tail) >= 3 and len(prev) >= 3:
        a = sum(d["total"] for d in tail) / len(tail)
        b = sum(d["total"] for d in prev) / len(prev)
        ta = sum(d["total"] for d in tail)
        tb = sum(d["total"] for d in prev)
        s["trend"] = {"recent": a, "before": b,
                      "pct": ((a - b) / b * 100.0) if b else 0,
                      "frag_recent": (sum(d["frag"] for d in tail) / ta * 100) if ta else 0,
                      "frag_before": (sum(d["frag"] for d in prev) / tb * 100) if tb else 0}

    # 每小时画像（跨天平均）
    on_sum = [0.0] * 24
    for d in active:
        for h in range(24):
            on_sum[h] += d["hour_on"][h]
    n = len(active) or 1
    s["hour_on_avg"] = [v / n for v in on_sum]
    s["hour_switch_avg"] = data["hourly_switch_avg"]
    busy_hours = [h for h in range(24) if s["hour_switch_avg"][h] > 0] or list(range(24))
    peak_h = max(busy_hours, key=lambda h: s["hour_switch_avg"][h])
    s["peak_hour"] = peak_h
    s["peak_hour_val"] = s["hour_switch_avg"][peak_h]
    quiet_hours = sorted(busy_hours, key=lambda h: s["hour_switch_avg"][h])
    s["quiet_hour"] = quiet_hours[0]
    s["quiet_hour_val"] = s["hour_switch_avg"][quiet_hours[0]]
    s["peak_on_hour"] = max(range(24), key=lambda h: s["hour_on_avg"][h])

    # 星期画像
    wd = {i: {"sn": 0, "switch": 0, "frag": 0, "on": 0.0, "on_n": 0} for i in range(7)}
    for d in days:
        w = d["date"].weekday()
        if d["total"] > 0:
            wd[w]["sn"] += 1
            wd[w]["switch"] += d["total"]
            wd[w]["frag"] += d["frag"]
        if d["on_min"] > 0:
            wd[w]["on_n"] += 1
            wd[w]["on"] += d["on_min"]
    s["weekday"] = {i: {"days": v["sn"], "on_days": v["on_n"],
                        "switch": v["switch"] / v["sn"] if v["sn"] else 0,
                        "on": v["on"] / v["on_n"] if v["on_n"] else 0,
                        "frag_pct": (v["frag"] / v["switch"] * 100) if v["switch"] else 0}
                    for i, v in wd.items()}

    # 应用时长 & 集中度
    apps = sorted(data["apps"].items(), key=lambda kv: -kv[1])
    s["app_top"] = apps[:10]
    s["app_total_min"] = sum(v for _, v in apps) / 60000.0
    s["app_focus_share"] = (apps[0][1] / 60000.0 / s["app_total_min"] * 100) \
        if apps and s["app_total_min"] else 0

    # 相关性：每小时在岗分钟 vs 该小时切换数
    s["corr"] = pearson([p[0] for p in data["scatter"]], [p[1] for p in data["scatter"]])

    # 异常日：切换数偏离均值 2σ
    vals = [d["total"] for d in active]
    if len(vals) >= 5:
        m = sum(vals) / len(vals)
        sd = (sum((v - m) ** 2 for v in vals) / len(vals)) ** 0.5
        s["outliers"] = [d for d in active if sd > 0 and abs(d["total"] - m) > 2 * sd]
    else:
        s["outliers"] = []

    # 会话集中度
    chats = sorted(data["chats"].items(), key=lambda kv: -kv[1])
    s["chat_top"] = chats[:12]
    s["chat_total"] = sum(v for _, v in chats)
    s["chat_share_top3"] = (sum(v for _, v in chats[:3]) / s["chat_total"] * 100) \
        if s["chat_total"] else 0
    s["chat_once"] = sum(1 for _, v in chats if v == 1)
    return s


# ---------------------------------------------------------------- 画图

def svg_open(width, height):
    return ('<svg viewBox="0 0 %d %d" width="100%%" height="%d" '
            'xmlns="http://www.w3.org/2000/svg" font-family="inherit">'
            % (width, height, height))


def axis_text(x, y, text, anchor="middle", size=10, color="#8b95a1"):
    return ('<text x="%.1f" y="%.1f" text-anchor="%s" font-size="%d" fill="%s">%s</text>'
            % (x, y, anchor, size, color, esc(text)))


def trend_chart(days, width=960, height=250):
    pad_l, pad_r, pad_t, pad_b = 46, 14, 14, 34
    iw, ih = width - pad_l - pad_r, height - pad_t - pad_b
    n = len(days)
    peak = max([d["total"] for d in days] + [1])
    avg7 = moving_average([d["total"] for d in days], 7)
    step = iw / max(1, n)
    bw = max(2.0, step * 0.6)

    def xc(i):
        return pad_l + step * (i + 0.5)

    def y(v):
        return pad_t + ih - ih * v / peak

    parts = [svg_open(width, height)]
    for k in range(0, 5):
        v = peak * k / 4.0
        yy = y(v)
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e8ecf1"/>'
                     % (pad_l, yy, width - pad_r, yy))
        parts.append(axis_text(pad_l - 6, yy + 3, "%d" % round(v), anchor="end"))
    for i, d in enumerate(days):
        h = ih * d["total"] / peak
        parts.append(
            '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="2" fill="#2a82da" opacity="%.2f">'
            '<title>%s %s&#10;有效切换 %d 次 · 碎片 %d 次（%.0f%%）&#10;在岗 %s</title></rect>'
            % (xc(i) - bw / 2, y(d["total"]), bw, max(0.0, h),
               0.28 if d["total"] == 0 else 0.78,
               d["date"].strftime("%m-%d"), WEEKDAY_CN[d["date"].weekday()],
               d["total"], d["frag"],
               (d["frag"] / d["total"] * 100) if d["total"] else 0,
               fmt_min(d["on_min"])))
    pts = " ".join("%.1f,%.1f" % (xc(i), y(v)) for i, v in enumerate(avg7))
    parts.append('<polyline points="%s" fill="none" stroke="#f0a24a" stroke-width="2"/>' % pts)
    every = max(1, n // 8)
    for i, d in enumerate(days):
        if i % every == 0 or i == n - 1:
            parts.append(axis_text(xc(i), height - 12, d["date"].strftime("%m-%d")))
    parts.append("</svg>")
    return "".join(parts)


def line_chart(values, labels, width=960, height=200, color="#c0392b", unit="%"):
    pad_l, pad_r, pad_t, pad_b = 46, 14, 14, 34
    iw, ih = width - pad_l - pad_r, height - pad_t - pad_b
    n = len(values)
    peak = max(values + [1]) * 1.15

    def x(i):
        return pad_l + (iw * i / max(1, n - 1))

    def y(v):
        return pad_t + ih - ih * v / peak

    parts = [svg_open(width, height)]
    for k in range(0, 4):
        v = peak * k / 3.0
        yy = y(v)
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e8ecf1"/>'
                     % (pad_l, yy, width - pad_r, yy))
        parts.append(axis_text(pad_l - 6, yy + 3, "%.0f%s" % (v, unit), anchor="end"))
    pts = [(x(i), y(v)) for i, v in enumerate(values)]
    parts.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="2"/>'
                 % (" ".join("%.1f,%.1f" % p for p in pts), color))
    for i, v in enumerate(values):
        parts.append('<circle cx="%.1f" cy="%.1f" r="3" fill="%s"><title>%s %s%.0f%s</title></circle>'
                     % (pts[i][0], pts[i][1], color, esc(labels[i]), "", v, unit))
    every = max(1, n // 12)
    for i, lb in enumerate(labels):
        if i % every == 0 or i == n - 1:
            parts.append(axis_text(pts[i][0], height - 12, lb))
    parts.append("</svg>")
    return "".join(parts)


def heatmap(days, width=960):
    left, top, ch = 52, 22, 18
    cw = (width - left - 8) / 24.0
    height = top + ch * len(days) + 26
    peak = max([max(d["hour_switches"]) for d in days] + [1])
    parts = [svg_open(width, height)]
    for h in range(24):
        if h % 3 == 0:
            parts.append(axis_text(left + cw * (h + 0.5), top - 8, "%02d" % h))
    for r, d in enumerate(days):
        yy = top + r * ch
        parts.append(axis_text(left - 8, yy + 13, d["date"].strftime("%m-%d"), anchor="end"))
        for h in range(24):
            v = d["hour_switches"][h]
            op = 0.0 if v == 0 else 0.12 + 0.88 * (v / peak) ** 0.65
            parts.append(
                '<rect x="%.1f" y="%.1f" width="%.1f" height="%d" fill="#2a82da" opacity="%.2f">'
                '<title>%s %s&#10;切换 %d 次 · 在岗 %s</title></rect>'
                % (left + cw * h + 0.6, yy + 1, cw - 1.2, ch - 2, op,
                   d["date"].strftime("%m-%d"), hour_label(h), v, fmt_min(d["hour_on"][h])))
    parts.append("</svg>")
    return "".join(parts), peak


def hour_bars(values, width=960, height=190, color="#2a82da", unit="",
              fmt=lambda v: "%.0f" % v):
    pad_l, pad_r, pad_t, pad_b = 46, 14, 14, 30
    iw, ih = width - pad_l - pad_r, height - pad_t - pad_b
    peak = max(values + [1]) * 1.12
    bw = iw / 24.0
    parts = [svg_open(width, height)]
    for k in range(0, 4):
        v = peak * k / 3.0
        yy = pad_t + ih - ih * v / peak
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e8ecf1"/>'
                     % (pad_l, yy, width - pad_r, yy))
        parts.append(axis_text(pad_l - 6, yy + 3, "%.0f%s" % (v, unit.strip()), anchor="end"))
    for h, v in enumerate(values):
        hh = ih * v / peak
        parts.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="2" fill="%s" opacity="%.2f">'
                     '<title>%s 平均 %s%s</title></rect>'
                     % (pad_l + bw * h + bw * 0.15, pad_t + ih - hh, bw * 0.7, max(0.0, hh),
                        color, 0.35 if v == 0 else 0.8, hour_label(h), fmt(v), unit))
        if h % 3 == 0:
            parts.append(axis_text(pad_l + bw * (h + 0.5), height - 10, "%02d" % h))
    parts.append("</svg>")
    return "".join(parts)


def hbars(items, width=470, color="#2a82da", unit_mins=False, suffix="", label_w=132):
    """items: [(label, value, tip)]，横向条形"""
    rowh = 24
    height = rowh * len(items) + 8
    peak = max([v for _, v, _ in items] + [1])
    parts = [svg_open(width, height)]
    for i, (label, v, tip) in enumerate(items):
        y = i * rowh + 4
        w = (width - label_w - 62) * v / peak
        shown = fmt_min(v) if unit_mins else ("%s%s" % (fmt_min(v), suffix) if suffix == " 分" else "%d%s" % (v, suffix))
        parts.append('<text x="%d" y="%d" text-anchor="end" font-size="%d" fill="#4a5560">%s</text>'
                     % (label_w - 8, y + 13, 11 if label_w >= 160 else 11, esc(label)))
        parts.append('<rect x="%d" y="%d" width="%.1f" height="15" rx="3" fill="%s" opacity="0.8">'
                     '<title>%s</title></rect>' % (label_w, y + 1, max(1.0, w), color, esc(tip)))
        parts.append('<text x="%.1f" y="%d" font-size="10" fill="#8b95a1">%s</text>'
                     % (label_w + max(1.0, w) + 6, y + 13, esc(shown)))
    parts.append("</svg>")
    return "".join(parts)


def hist_chart(hist, width=960, height=210, color="#2a82da"):
    pad_l, pad_r, pad_t, pad_b = 46, 14, 14, 34
    iw, ih = width - pad_l - pad_r, height - pad_t - pad_b
    peak = max(hist + [1]) * 1.12
    n = len(hist)
    bw = iw / n
    total = sum(hist) or 1
    parts = [svg_open(width, height)]
    for k in range(0, 4):
        v = peak * k / 3.0
        yy = pad_t + ih - ih * v / peak
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e8ecf1"/>'
                     % (pad_l, yy, width - pad_r, yy))
        parts.append(axis_text(pad_l - 6, yy + 3, "%d" % round(v), anchor="end"))
    for i, v in enumerate(hist):
        hh = ih * v / peak
        parts.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="2" fill="%s" opacity="0.8">'
                     '<title>%s&#10;%d 次 · 占 %.1f%%</title></rect>'
                     % (pad_l + bw * i + bw * 0.18, pad_t + ih - hh, bw * 0.64, max(0.0, hh),
                        color, esc(DWELL_BUCKETS[i][2]), v, v / total * 100))
        parts.append(axis_text(pad_l + bw * (i + 0.5), height - 22, DWELL_BUCKETS[i][2], size=9))
        pct = v / total * 100
        shown = "<1%" if 0 < pct < 1 else "%.0f%%" % pct
        parts.append(axis_text(pad_l + bw * (i + 0.5), height - 10, shown, size=9, color="#b0b8c1"))
    parts.append("</svg>")
    return "".join(parts)


def scatter_chart(points, width=960, height=240):
    pad_l, pad_r, pad_t, pad_b = 46, 16, 14, 34
    iw, ih = width - pad_l - pad_r, height - pad_t - pad_b
    mx = max([p[0] for p in points] + [1]) * 1.05
    my = max([p[1] for p in points] + [1]) * 1.05
    parts = [svg_open(width, height)]
    for k in range(0, 4):
        v = mx * k / 3.0
        xx = pad_l + iw * k / 3.0
        parts.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="#e8ecf1"/>'
                     % (xx, pad_t, xx, pad_t + ih))
        parts.append(axis_text(xx, height - 16, "%.0f" % v))
    for k in range(0, 4):
        v = my * k / 3.0
        yy = pad_t + ih - ih * k / 3.0
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e8ecf1"/>'
                     % (pad_l, yy, width - pad_r, yy))
        parts.append(axis_text(pad_l - 6, yy + 3, "%.0f" % v, anchor="end"))
    for x, y, date, h in points:
        parts.append('<circle cx="%.1f" cy="%.1f" r="3.2" fill="#2a82da" opacity="0.45">'
                     '<title>%s %s&#10;在岗 %.0f 分 · 切换 %d 次</title></circle>'
                     % (pad_l + iw * x / mx, pad_t + ih - ih * y / my, esc(date), hour_label(h), x, y))
    parts.append(axis_text(pad_l + iw / 2, height - 2, "该小时在岗分钟", size=10))
    parts.append("</svg>")
    return "".join(parts)


# ---------------------------------------------------------------- 结论

def verdicts(s, data):
    out = []
    if s["trend"]:
        t = s["trend"]
        pct = abs(t["pct"])
        if pct < 8:
            out.append("最近 7 天日均 %.0f 次，和前 7 天（%.0f 次）基本持平。"
                       % (t["recent"], t["before"]))
        elif t["pct"] > 0:
            out.append("最近 7 天日均切换 <b>%.0f 次</b>，比前 7 天的 %.0f 次<b>多了 %.0f%%</b>，切换更碎了。"
                       % (t["recent"], t["before"], pct))
        else:
            out.append("最近 7 天日均切换 <b>%.0f 次</b>，比前 7 天的 %.0f 次<b>少了 %.0f%%</b>，正在变得更连贯。"
                       % (t["recent"], t["before"], pct))
    else:
        out.append("每组至少要有 3 天带切换记录才能比较「最近 7 天 vs 前 7 天」，目前样本不够，"
                   "先看当前水平。")

    out.append("碎片切换（上一屏待不到 10 秒就走）占 <b>%.0f%%</b>：%s"
               % (s["frag_pct"],
                  "三次切换里快有两次是纯打断，手头的活很难连贯。"
                  if s["frag_pct"] >= 50 else
                  ("超过一半，属于典型的中断型工作日。" if s["frag_pct"] >= 50 else
                   ("接近一半，属于典型的中断型工作日。" if s["frag_pct"] >= 35 else
                   "比例不高，多数切换背后还是有实际内容的。"))))

    if s["focus_blocks"]:
        out.append("有切换记录的 %d 天里，连续专注 15 分钟以上共 <b>%d 段</b>，日均 %.1f 段、%.0f 分钟；最长一段 <b>%s</b>。"
                   % (s["sw_days"], s["focus_blocks"], s["focus_avg"],
                      s["focus_min"] / max(1, s["sw_days"]), fmt_min(s["longest_focus"])))

    if data["enter"]:
        top = sorted(data["enter"].items(), key=lambda kv: -kv[1])[:3]
        out.append("最常被切进去的是 %s，这些就是你的打断来源。"
                   % "、".join("<b>%s</b> %d 次" % (esc(k), v) for k, v in top))

    if s["peak_hour_val"]:
        out.append("切换最密的时段是 <b>%s 前后</b>（平均每小时 %.1f 次），"
                   "有记录的小时里最安静的是 %s（%.1f 次）。"
                   % (hour_label(s["peak_hour"]), s["peak_hour_val"],
                      hour_label(s["quiet_hour"]), s["quiet_hour_val"]))

    if s["corr"] is not None:
        c = s["corr"]
        if c >= 0.5:
            out.append("每小时切换数和当小时在岗时长强相关（r=%.2f）：切换多的时候正是活最多的时候，不是单纯在乱点。" % c)
        elif c <= -0.3:
            out.append("切换数和在岗时长负相关（r=%.2f）：闲的时候切得更勤，忙起来反而坐得住。" % c)
        else:
            out.append("切换数与在岗时长相关性很弱（r=%.2f），频繁切换不是工时长短造成的。" % c)

    if s["app_top"]:
        name, ms = s["app_top"][0]
        out.append("在岗时间里 %.0f%% 花在 <b>%s</b>（%s），后面是 %s。"
                   % (s["app_focus_share"], esc(name), fmt_min(ms / 60000.0, short=True),
                      "、".join(esc(n) for n, _ in s["app_top"][1:4])))

    if s["chat_total"]:
        out.append("企业微信里前 3 个会话吃掉 %.0f%% 的会话切换，对话高度集中在这几个人/群上。"
                   % s["chat_share_top3"])
    return out


# ---------------------------------------------------------------- 页面

CSS = """
:root{--ink:#20262e;--dim:#6b7683;--line:#e6eaef;--blue:#2a82da;--bg:#f6f8fa;}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
 font-family:"Noto Sans CJK SC","Source Han Sans SC","Microsoft YaHei",system-ui,sans-serif;
 font-size:14px;line-height:1.6;}
.wrap{max-width:1040px;margin:0 auto;padding:28px 20px 60px;}
h1{font-size:22px;margin:0 0 4px;font-weight:600;}
.sub{color:var(--dim);font-size:12.5px;margin-bottom:18px;}
h2{font-size:15px;margin:0 0 2px;font-weight:600;}
.hint{color:var(--dim);font-size:12px;margin:0 0 12px;}
.card{background:#fff;border:1px solid var(--line);border-radius:10px;padding:16px 18px;margin-bottom:16px;}
.kpis{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-bottom:16px;}
.kpi{background:#fff;border:1px solid var(--line);border-radius:10px;padding:12px 14px;}
.kpi .k{color:var(--dim);font-size:11.5px;}
.kpi .v{font-size:23px;font-weight:600;letter-spacing:-0.3px;margin:2px 0 1px;}
.kpi .v .u{font-size:12px;font-weight:400;color:var(--dim);margin-left:3px;}
.kpi .n{color:var(--dim);font-size:11px;}
.verdict{background:#fff;border:1px solid var(--line);border-left:3px solid var(--blue);
 border-radius:8px;padding:14px 18px;margin-bottom:16px;font-size:13.5px;}
.verdict b{color:#1668b8;font-weight:600;}
.verdict ul{margin:0;padding-left:18px}.verdict li{margin:5px 0;}
table{width:100%;border-collapse:collapse;font-size:12.5px;font-variant-numeric:tabular-nums;}
th,td{padding:6px 8px;border-bottom:1px solid var(--line);text-align:left;white-space:nowrap;}
th{color:var(--dim);font-weight:500;font-size:11.5px;}
td.num,th.num{text-align:right;}
tbody tr:hover{background:#f3f8fe;}
tr.hl td{background:#fff8ec;font-weight:600;}
.nav{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:18px;}
.nav a{font-size:12px;color:#1668b8;text-decoration:none;border:1px solid var(--line);
 background:#fff;border-radius:999px;padding:3px 11px;}
.nav a:hover{background:#eff6ff;}
.legend{display:flex;align-items:center;gap:6px;color:var(--dim);font-size:11.5px;margin-top:6px;}
.dot{display:inline-block;width:9px;height:9px;border-radius:2px;margin-right:4px;}
.two{display:grid;grid-template-columns:1fr 1fr;gap:16px;}
@media(max-width:860px){.kpis{grid-template-columns:repeat(2,1fr)}.two{grid-template-columns:1fr}}
"""


def kpi(label, value, unit="", note=""):
    return ('<div class="kpi"><div class="k">%s</div><div class="v">%s<span class="u">%s</span></div>'
            '<div class="n">%s</div></div>' % (esc(label), esc(value), esc(unit), note))


def render(data, s, title, start, end):
    days = data["days"]
    heat_svg, heat_peak = heatmap(days)

    frag_vals = [(d["frag"] / d["total"] * 100) if d["total"] else 0 for d in days]
    frag_labels = [d["date"].strftime("%m-%d") for d in days]

    app_items = [(n, ms / 60000.0, "%s %s" % (n, fmt_min(ms / 60000.0, short=True)))
                 for n, ms in s["app_top"]]
    enter_items = [(n, v, "%s 被切进 %d 次" % (n, v))
                   for n, v in sorted(data["enter"].items(), key=lambda kv: -kv[1])[:10]]
    pair_items = [("%s → %s" % (a, b), v, "%s → %s：%d 次" % (a, b, v))
                  for (a, b), v in sorted(data["pairs"].items(), key=lambda kv: -kv[1])[:10]]
    def short(name, limit=16):
        return name if len(name) <= limit else name[:limit] + "…"

    chat_items = [(short(n), v, "%s：%d 次，出现在 %d 天里" % (n, v, data["chat_days"].get(n, 0)))
                  for n, v in s["chat_top"]]

    dwell_items = []
    for n, cnt in data["leave"].items():
        sec = data["leave_sec"].get(n, 0)
        if cnt < 5:          # 样本太少的应用平均没有意义
            continue
        dwell_items.append((n, sec / cnt, cnt))
    dwell_items.sort(key=lambda x: -x[1])
    dwell_items = [(n, avg, "%s：平均待 %s（离开 %d 次）" % (n, fmt_min(avg / 60.0, short=True), cnt))
                   for n, avg, cnt in dwell_items[:10]]

    peak_wd = max(range(7), key=lambda i: s["weekday"][i]["switch"])
    wd_rows = []
    for i in range(7):
        w = s["weekday"][i]
        cls = ' class="hl"' if i == peak_wd and w["days"] else ""
        wd_rows.append(
            "<tr%s><td>%s</td><td class='num'>%d</td><td class='num'>%.0f</td>"
            "<td class='num'>%s</td><td class='num'>%.0f%%</td></tr>"
            % (cls, WEEKDAY_CN[i], w["days"], w["switch"],
               fmt_min(w["on"], short=True) if w["days"] else "-", w["frag_pct"]))

    rows = []
    for d in days:
        if not (d["total"] or d["on_min"]):
            continue
        rows.append(
            "<tr><td>%s %s</td><td class='num'>%d</td><td class='num'>%d</td>"
            "<td class='num'>%d</td><td class='num'>%.0f%%</td><td class='num'>%.0f 秒</td>"
            "<td class='num'>%d</td><td class='num'>%s</td><td class='num'>%s</td>"
            "<td class='num'>%s</td><td class='num'>%d</td><td class='num'>%d</td></tr>"
            % (d["date"].strftime("%m-%d"), WEEKDAY_CN[d["date"].weekday()],
               d["total"], d["raw"], d["frag"],
               (d["frag"] / d["total"] * 100) if d["total"] else 0,
               d["median_dwell"], d["focus_blocks"],
               fmt_min(d["longest_focus"], short=True),
               fmt_min(d["on_min"], short=True), fmt_min(d["lock_min"], short=True),
               d["chats"], d["events"]))

    if data["events"]:
        events_html = ("<table><thead><tr><th>时间</th><th>说明</th><th class='num'>时长</th>"
                       "</tr></thead><tbody>%s</tbody></table>"
                       % "".join("<tr><td>%s %s</td><td>%s</td><td class='num'>%d 分</td></tr>"
                                 % (e["date"], e["when"].strftime("%H:%M"),
                                    esc(e["label"]), e["minutes"]) for e in data["events"]))
    else:
        events_html = '<p class="hint">还没有手工记录的事件。</p>'

    outliers_html = ""
    if s["outliers"]:
        outliers_html = "<p class='hint' style='margin:8px 0 0'>异常日（切换数偏离均值 2σ 以上）：%s</p>" % "、".join(
            "%s（%d 次）" % (d["date"].strftime("%m-%d"), d["total"]) for d in s["outliers"])

    longest_day = max(days, key=lambda d: d["longest_focus"]) if s["longest_focus"] else None

    return """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>%(title)s</title><style>%(css)s</style></head>
<body><div class="wrap">
<h1>%(title)s</h1>
<div class="sub">%(start)s ~ %(end)s · 共 %(ndays)d 天，有切换记录 %(swdays)d 天、有用时记录 %(ondays)d 天、有锁屏记录 %(lockdays)d 天 · 生成于 %(now)s · 数据只在本机</div>

<div class="nav">
 <a href="#verdict">结论</a><a href="#kpi">指标</a><a href="#trend">切换趋势</a>
 <a href="#frag">碎片化</a><a href="#heat">时段分布</a><a href="#hours">作息</a>
 <a href="#apps">应用</a><a href="#dwell">停留时长</a><a href="#focus">专注</a>
 <a href="#chat">企业微信</a><a href="#weekday">星期</a><a href="#event">事件</a>
 <a href="#detail">明细</a><a href="#method">口径</a>
</div>

<div class="card" id="verdict"><h2>结论</h2>
<div class="verdict" style="border:none;padding:4px 0;margin:0"><ul>%(bullets)s</ul></div></div>

<div class="kpis" id="kpi">
 %(kpi1)s %(kpi2)s %(kpi3)s %(kpi4)s %(kpi5)s %(kpi6)s %(kpi7)s %(kpi8)s
</div>

<div class="card" id="trend">
 <h2>每天切换次数</h2>
 <p class="hint">蓝柱＝当天有效切换数，橙线＝7 天滑动平均（按自然日算，没记录的日子记 0）。悬停看当天的碎片与在岗时长。</p>
 %(trend)s
 <div class="legend"><span><i class="dot" style="background:#2a82da"></i>每天有效切换</span>
 <span><i class="dot" style="background:#f0a24a"></i>7 天滑动平均</span></div>
 %(outliers)s
</div>

<div class="card" id="frag">
 <h2>碎片化程度</h2>
 <p class="hint">每天「上一屏待不到 10 秒就切走」的切换占比。这条线往上走，说明打断更密、更坐不住。</p>
 %(frag)s
</div>

<div class="card" id="heat">
 <h2>24 小时 × 日期的热力图</h2>
 <p class="hint">颜色越深＝那个小时切换越多。横向看一天里的波峰，纵向看日子之间的稳定程度。</p>
 %(heat)s
 <div class="legend"><span>少</span>
 <span><i class="dot" style="background:#2a82da;opacity:.15"></i><i class="dot" style="background:#2a82da;opacity:.4"></i><i class="dot" style="background:#2a82da;opacity:.7"></i><i class="dot" style="background:#2a82da;opacity:1"></i></span>
 <span>多（单小时最多 %(heatmax)d 次）</span></div>
</div>

<div class="card" id="hours">
 <h2>作息画像</h2>
 <p class="hint">上：平均每小时在岗分钟（跨 %(ondays)d 个有记录的日子）；下：平均每小时切换次数。</p>
 %(hour_on)s
 <p class="hint" style="margin:10px 0 8px">切换高点在 <b>%(peakhour)s</b> 前后（平均每小时 %(peakv).1f 次）；在岗时长的最高点在 <b>%(peakon)s</b>。</p>
 %(hour_sw)s
</div>

<div class="card" id="apps">
 <h2>应用时长 TOP10</h2>
 <p class="hint">统计期内累计的操作 + 查看时长，第一名占全部在岗时间的 %(share).0f%%。</p>
 %(apps)s
 <div class="two" style="margin-top:20px">
  <div><h2 style="font-size:13px">被切进去的次数</h2>
   <p class="hint">被打断时你切进去的那个应用，次数越多越是打断源。</p>%(enter)s</div>
  <div><h2 style="font-size:13px">切换路径 TOP10</h2>
   <p class="hint">从哪个应用切到哪个应用最频繁。</p>%(pairs)s</div>
 </div>
</div>

<div class="card" id="dwell">
 <h2>每次切换前，你在一屏待了多久</h2>
 <p class="hint">分布越靠左，「扫一眼就走」越多；右半边（15 分钟以上）是能连起来干活的时段。</p>
 %(hist)s
</div>

<div class="card" id="focus">
 <h2>专注与停留</h2>
 <p class="hint">左：各应用平均待多久才切走，越短越像「过一下」。右：专注块统计。</p>
 <div class="two">
  <div>%(dwells)s</div>
  <div style="padding-top:4px">
   <p class="hint" style="margin:0 0 10px">
   连续待在同一上下文 15–60 分钟算一段专注块。<br>
   共 <b>%(fb)d 段</b>，合计 %(fm)s，日均 <b>%(fa).1f 段</b>。<br>
   最长一段 <b>%(lf)s</b>%(longestday)s。
   </p>
   <p class="hint" style="margin:0">专注块越少，说明切换碎到 15 分钟都连不起来。</p>
  </div>
 </div>
</div>

<div class="card" id="chat">
 <h2>企业微信：最常切进去的会话</h2>
 <p class="hint">切进某个具体会话就算一次；前 3 个会话占全部会话切换的 %(share3).0f%%（共 %(chat_total)d 次）。会话名靠截图 OCR 识别，可能有误差；只出现过 1 次的有 %(chat_once)d 个，多半是识别噪声。</p>
 %(chat)s
</div>

<div class="card" id="weekday">
 <h2>星期几最容易被切碎</h2>
 <p class="hint">只统计有记录的天，切换数与在岗时长取当天平均；高亮行是切换最多的一天。
 切换记录只覆盖 %(swdays)s 天，每个星期几的样本大多是 1–2 天，先当参考。</p>
 <table><thead><tr><th>星期</th><th class="num">有记录天数</th><th class="num">日均切换</th>
 <th class="num">日均在岗</th><th class="num">碎片率</th></tr></thead><tbody>%(wd)s</tbody></table>
</div>

<div class="card" id="event">
 <h2>外部事件</h2>
 <p class="hint">手工记的会议、被叫走、接电话等。共 %(en)d 次、%(em)s。</p>
 %(events)s
</div>

<div class="card" id="detail">
 <h2>逐日明细</h2>
 <p class="hint">「有效切换」是去掉抖动和系统弹层后的数，「原始」是库里的全部记录。</p>
 <table><thead><tr><th>日期</th><th class="num">有效切换</th><th class="num">原始</th>
 <th class="num">碎片</th><th class="num">碎片率</th><th class="num">停留中位数</th>
 <th class="num">专注块</th><th class="num">最长专注</th><th class="num">在岗</th>
 <th class="num">锁屏</th><th class="num">会话</th><th class="num">事件</th></tr></thead>
 <tbody>%(rows)s</tbody></table>
</div>

<div class="card" id="method">
 <h2>统计口径</h2>
 <div class="hint" style="margin:0">
  <b>有效切换</b>＝原始记录去掉：① 同一应用内换窗口/换页（Chrome 换标签、资源管理器换目录，
  kind=inner，单独计数）；② 上一屏停留不到 3 秒就切走（焦点/OCR 抖动）；
  ③ 间隔 20 分钟以上（午休、长会后回来，且 dwell 本身在采集端已截到 20 分钟）；
  ④ 系统弹层（任务栏、控制中心、选择文件对话框、剪贴板）。<br>
  <b>碎片切换</b>＝有效切换里上一屏停留不到 10 秒的。<b>专注块</b>＝同一上下文连续停留 15–60 分钟（超过 1 小时视为走开，不计）。<br>
  <b>在岗</b>＝操作 + 查看时长（不含锁屏）；<b>会话</b>＝当天切进去过的企业微信不同会话数。<br>
  <b>相关性</b>＝「每小时切换数」与「该小时在岗分钟」的皮尔逊相关系数，只用有活动的小时。<br>
  原始记录一条未删，数据库和导出 CSV 里都在。
 </div>
</div>

</div></body></html>
""" % {
        "title": esc(title), "css": CSS, "start": start.strftime("%Y-%m-%d"),
        "end": end.strftime("%Y-%m-%d"), "ndays": s["days"], "active": s["active_days"],
        "swdays": s["sw_days"], "ondays": s["on_days"], "lockdays": s["lock_days"],
        "now": dt.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "bullets": "".join("<li>%s</li>" % v for v in verdicts(s, data)),
        "kpi1": kpi("日均有效切换", "%.0f" % s["avg"], "次", "按有切换记录的 %d 天平均" % s["sw_days"]),
        "kpi2": kpi("碎片占比", "%.0f" % s["frag_pct"], "%", "共 %d 次「扫一眼就走」" % s["frag"]),
        "kpi3": kpi("日均在岗", fmt_min(s["on_avg"], short=True), "",
                    "操作 %.0f%% · 查看 %.0f%%" % (
                        s["active_min"] / max(1, s["on_min"]) * 100,
                        s["viewing_min"] / max(1, s["on_min"]) * 100)),
        "kpi4": kpi("日均锁屏（含离开）", fmt_min(s["lock_min"] / max(1, s["lock_days"]), short=True), "",
                    "占统计日 %.0f%% · 有记录 %d 天"
                    % (s["lock_min"] / max(1, s["lock_days"]) / 1440.0 * 100, s["lock_days"])),
        "kpi5": kpi("专注块", "%d" % s["focus_blocks"], "段",
                    "日均 %.1f 段 · 最长 %s" % (s["focus_avg"], fmt_min(s["longest_focus"], short=True))),
        "kpi6": kpi("最忙一天", "%d" % (s["busiest"]["total"] if s["busiest"] else 0), "次",
                    s["busiest"]["date"].strftime("%m-%d") if s["busiest"] else ""),
        "kpi7": kpi("单日最多会话", "%d" % s["chats"], "个",
                    "累计 %d 次会话切换" % s["chat_total"]),
        "kpi8": kpi("外部事件", "%d" % s["events"], "次", "共 %s" % fmt_min(s["event_min"], short=True)),
        "trend": trend_chart(days),
        "frag": line_chart(frag_vals, frag_labels, color="#c0392b", unit="%"),
        "heat": heat_svg, "heatmax": heat_peak,
        "hour_on": hour_bars(s["hour_on_avg"], color="#2a82da", unit=" 分"),
        "hour_sw": hour_bars(s["hour_switch_avg"], color="#ef8a3c", unit=" 次", fmt=lambda v: "%.2f" % v),
        "peakhour": hour_label(s["peak_hour"]), "peakv": s["peak_hour_val"],
        "peakon": hour_label(s["peak_on_hour"]),
        "apps": hbars(app_items, unit_mins=True),
        "share": s["app_focus_share"],
        "enter": hbars(enter_items, color="#4a9de0", suffix=" 次"),
        "pairs": hbars(pair_items, color="#8e7cc3", suffix=" 次", label_w=196),
        "hist": hist_chart(data["hist"]),
        "dwells": hbars(dwell_items, color="#3aa17e", suffix=" 秒"),
        "fb": s["focus_blocks"], "fm": fmt_min(s["focus_min"], short=True), "fa": s["focus_avg"],
        "lf": fmt_min(s["longest_focus"], short=True),
        "longestday": ("（%s）" % longest_day["date"].strftime("%m-%d")) if longest_day else "",
        "chat": hbars(chat_items, color="#2fa8a0", suffix=" 次"),
        "share3": s["chat_share_top3"], "chat_total": s["chat_total"],
        "chat_once": s["chat_once"],
        "wd": "".join(wd_rows),
        "en": s["events"], "em": fmt_min(s["event_min"], short=True),
        "events": events_html, "rows": "".join(rows), "outliers": outliers_html,
    }


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description="生成应用使用 / 上下文切换的本地网页报告")
    ap.add_argument("--db", default=DB_DEFAULT)
    ap.add_argument("--days", type=int, default=30, help="最近多少天（默认 30）")
    ap.add_argument("--from", dest="start", help="起始日期 YYYY-MM-DD")
    ap.add_argument("--to", dest="end", help="结束日期 YYYY-MM-DD")
    ap.add_argument("--out", default=os.path.expanduser("~/下载/app-usage-switch-report.html"))
    args = ap.parse_args(argv)

    end = dt.date.fromisoformat(args.end) if args.end else dt.date.today()
    start = dt.date.fromisoformat(args.start) if args.start else end - dt.timedelta(days=args.days - 1)
    if start > end:
        print("起始日期晚于结束日期", file=sys.stderr)
        return 2
    if not os.path.exists(args.db):
        print("找不到数据库：%s" % args.db, file=sys.stderr)
        return 2

    data = load(args.db, start, end)
    if not any(d["total"] or d["on_min"] for d in data["days"]):
        print("%s ~ %s 之间没有数据" % (start, end), file=sys.stderr)
        return 1

    s = summarize(data)
    out = os.path.expanduser(args.out)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(render(data, s, "使用与上下文切换报告", start, end))

    print("已生成：%s" % out)
    print("%s ~ %s：日均 %.0f 次切换，碎片 %.0f%%，日均在岗 %s，专注块 %d 段"
          % (start, end, round(s["avg"]), s["frag_pct"],
             fmt_min(s["on_avg"], short=True), s["focus_blocks"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
