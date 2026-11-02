#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""app-usage-monitor-report — 把上下文切换数据画成一张本地网页月报

用法：
    app-usage-monitor-report                 # 最近 30 天，写到 ~/下载/app-usage-switch-report.html
    app-usage-monitor-report --days 14
    app-usage-monitor-report --from 2026-10-01 --to 2026-10-31 --out /tmp/report.html

只读本地数据库（默认 ~/.local/share/app-usage-monitor/usage.db），生成的 HTML
自带全部样式与脚本、不加载任何外部资源，断网也能看，直接双击打开即可。
"""

import argparse
import datetime as dt
import os
import sqlite3
import sys

DB_DEFAULT = os.path.expanduser("~/.local/share/app-usage-monitor/usage.db")
WXWORK_NAMES = ("企业微信", "wxwork")

# 统计口径：原始数据里混着不少「假切换」，月报按下面的规则算一遍干净的
MIN_DWELL_SEC = 3      # 上一屏待不到 3 秒就切走，是焦点抖动/OCR 抖，不算一次切换
SESSION_GAP_SEC = 1200  # 间隔 20 分钟以上算离开后回来（午休、长会），不接着算
SYSTEM_APPS = ("dde-dock", "深度控制中心", "dde-select-dialog", "剪贴板", "dde-osd",
               "Dde-select-dialog", "dde-session-ui", "深度文件管理器预览")


# ---------------------------------------------------------------- 数据

def load(db_path, start, end):
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    con.row_factory = sqlite3.Row

    start_ts = int(dt.datetime.combine(start, dt.time(0, 0)).timestamp())
    end_ts = int(dt.datetime.combine(end, dt.time(23, 59, 59)).timestamp())

    days = []
    day = start
    while day <= end:
        days.append(day)
        day += dt.timedelta(days=1)

    per_day = {d.isoformat(): {"total": 0, "raw": 0, "frag": 0, "hours": [0] * 24,
                               "dwell": [], "chats": set(), "apps": {}}
               for d in days}

    rows = con.execute(
        "SELECT ts, kind, from_app, to_app, from_ctx, to_ctx, dwell_sec "
        "FROM switches WHERE ts BETWEEN ? AND ? ORDER BY ts", (start_ts, end_ts))
    for r in rows:
        key = dt.datetime.fromtimestamp(r["ts"]).date().isoformat()
        rec = per_day.get(key)
        if rec is None:
            continue
        rec["raw"] += 1
        dwell = r["dwell_sec"] or 0
        to_app = r["to_app"] or ""
        from_app = r["from_app"] or ""
        if any(a in to_app or a in from_app for a in SYSTEM_APPS):
            continue
        if dwell < MIN_DWELL_SEC or dwell >= SESSION_GAP_SEC:
            continue
        rec["total"] += 1
        if dwell < 10:
            rec["frag"] += 1
        rec["hours"][dt.datetime.fromtimestamp(r["ts"]).hour] += 1
        if r["dwell_sec"]:
            rec["dwell"].append(r["dwell_sec"])
        to_app = r["to_app"] or ""
        rec["apps"][to_app] = rec["apps"].get(to_app, 0) + 1
        ctx = r["to_ctx"] or ""
        if any(w in to_app for w in WXWORK_NAMES) and ctx and ctx not in WXWORK_NAMES \
                and not any(w in ctx for w in WXWORK_NAMES):
            rec["chats"].add(ctx)

    events = {d.isoformat(): {"n": 0, "minutes": 0} for d in days}
    for r in con.execute(
            "SELECT date, start_ts, end_ts FROM events WHERE date BETWEEN ? AND ?",
            (start.isoformat(), end.isoformat())):
        rec = events.get(r["date"])
        if rec:
            rec["n"] += 1
            rec["minutes"] += max(0, (r["end_ts"] or 0) - (r["start_ts"] or 0)) // 60

    con.close()

    out = []
    for d in days:
        rec = per_day[d.isoformat()]
        dwell = rec["dwell"]
        out.append({
            "date": d,
            "total": rec["total"],
            "raw": rec["raw"],
            "frag": rec["frag"],
            "median_dwell": sorted(dwell)[len(dwell) // 2] if dwell else 0,
            "chats": len(rec["chats"]),
            "apps": rec["apps"],
            "hours": rec["hours"],
            "events": events[d.isoformat()]["n"],
            "event_min": events[d.isoformat()]["minutes"],
        })
    return out


# ---------------------------------------------------------------- 计算

def moving_average(values, window=7):
    out = []
    for i in range(len(values)):
        lo = max(0, i - window + 1)
        chunk = values[lo:i + 1]
        out.append(sum(chunk) / len(chunk))
    return out


def summarize(days):
    active = [d for d in days if d["total"] > 0]
    total = sum(d["total"] for d in days)
    frag = sum(d["frag"] for d in days)
    chats = max((d["chats"] for d in days), default=0)
    busiest = max(days, key=lambda d: d["total"]) if days else None
    calmest = min(active, key=lambda d: d["total"]) if active else None

    # 趋势：最后 7 天 vs 前面 7 天（都只算有记录的天）
    tail = [d for d in days[-7:] if d["total"] > 0]
    prev = [d for d in days[-14:-7] if d["total"] > 0]
    trend = None
    if tail and prev:
        a = sum(d["total"] for d in tail) / len(tail)
        b = sum(d["total"] for d in prev) / len(prev)
        if b > 0:
            trend = {"recent": a, "before": b, "pct": (a - b) / b * 100.0}

    return {
        "days": len(days),
        "active_days": len(active),
        "total": total,
        "raw": sum(d["raw"] for d in days),
        "avg": total / len(active) if active else 0,
        "frag": frag,
        "frag_pct": (frag / total * 100.0) if total else 0,
        "chats": chats,
        "busiest": busiest,
        "calmest": calmest,
        "events": sum(d["events"] for d in days),
        "event_min": sum(d["event_min"] for d in days),
        "trend": trend,
    }


# ---------------------------------------------------------------- 画图

def line_chart(days, width=920, height=260):
    pad_l, pad_r, pad_t, pad_b = 44, 16, 16, 30
    inner_w = width - pad_l - pad_r
    inner_h = height - pad_t - pad_b
    n = len(days)
    peak = max([d["total"] for d in days] + [1])
    avg7 = moving_average([d["total"] for d in days], 7)

    def x(i):
        return pad_l + (inner_w * i / max(1, n - 1))

    def y(v):
        return pad_t + inner_h - inner_h * v / peak

    bars, dots, ma = [], [], []
    bw = max(2.0, inner_w / max(1, n) * 0.55)
    for i, d in enumerate(days):
        h = inner_h * d["total"] / peak
        bars.append(
            '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="2" fill="#2a82da" opacity="%.2f">'
            '<title>%s │ 切换 %d 次 · 碎片 %d 次</title></rect>'
            % (x(i) - bw / 2, y(d["total"]), bw, max(0.0, h),
               0.35 if d["total"] == 0 else 0.75,
               d["date"].strftime("%m-%d"), d["total"], d["frag"]))
        if d["total"]:
            dots.append('<circle cx="%.1f" cy="%.1f" r="2.2" fill="#1b5f9e" opacity="0.9"/>'
                        % (x(i), y(d["total"])))
    points = " ".join("%.1f,%.1f" % (x(i), y(v)) for i, v in enumerate(avg7))
    ma.append('<polyline points="%s" fill="none" stroke="#f0a24a" stroke-width="2.4" '
              'stroke-linejoin="round"/>' % points)

    grid, labels = [], []
    for frac in (0, 0.25, 0.5, 0.75, 1.0):
        v = peak * frac
        yy = y(v)
        grid.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e8eaed" stroke-width="1"/>'
                    % (pad_l, yy, width - pad_r, yy))
        labels.append('<text x="%d" y="%.1f" text-anchor="end" font-size="10" fill="#8b9096">%d</text>'
                      % (pad_l - 6, yy + 3.5, round(v)))

    step = max(1, n // 10)
    for i in range(0, n, step):
        labels.append('<text x="%.1f" y="%d" text-anchor="middle" font-size="10" fill="#8b9096">%s</text>'
                      % (x(i), height - 10, days[i]["date"].strftime("%m-%d")))

    return ('<svg viewBox="0 0 %d %d" width="100%%" height="%d" role="img" '
            'aria-label="每日切换次数趋势">%s%s%s%s</svg>'
            % (width, height, height, "".join(grid), "".join(bars), "".join(ma), "".join(labels)))


def heatmap(days, width=920):
    n = len(days)
    cell_h, cell_w = 11, max(6, min(22, (width - 60) // max(1, n)))
    left, top = 34, 14
    peak = max([max(d["hours"]) for d in days] + [1])
    cells = []
    for hi in range(24):
        for di, d in enumerate(days):
            v = d["hours"][hi]
            if v == 0:
                continue
            alpha = 0.15 + 0.85 * (v / peak)
            cells.append('<rect x="%d" y="%d" width="%d" height="%d" rx="1.5" fill="#2a82da" '
                         'opacity="%.2f"><title>%s %02d:00 │ %d 次</title></rect>'
                         % (left + di * cell_w, top + hi * cell_h, max(2, cell_w - 1), cell_h - 1,
                            alpha, d["date"].strftime("%m-%d"), hi, v))
    axis = []
    for hi in (0, 6, 12, 18, 23):
        axis.append('<text x="%d" y="%d" text-anchor="end" font-size="9" fill="#8b9096">%02d</text>'
                    % (left - 6, top + hi * cell_h + 8, hi))
    ticks = []
    step = max(1, n // 8)
    for di in range(0, n, step):
        ticks.append('<text x="%d" y="%d" text-anchor="middle" font-size="9" fill="#8b9096">%s</text>'
                     % (left + di * cell_w + cell_w / 2, top + 24 * cell_h + 12,
                        days[di]["date"].strftime("%m-%d")))
    height = top + 24 * cell_h + 22
    return ('<svg viewBox="0 0 %d %d" width="100%%" height="%d">%s%s%s</svg>'
            % (left + n * cell_w + 10, height, height, "".join(cells), "".join(axis), "".join(ticks)))


def table(days):
    rows = []
    for d in reversed(days):
        if d["total"] == 0 and d["events"] == 0:
            continue
        rows.append(
            "<tr><td>%s</td><td class='num'>%d</td><td class='num'>%d</td><td class='num'>%d</td>"
            "<td class='num'>%.0f%%</td><td class='num'>%d</td><td class='num'>%d</td>"
            "<td class='num'>%d</td><td class='num'>%d</td></tr>"
            % (d["date"].strftime("%Y-%m-%d %a"), d["total"], d["raw"], d["frag"],
               (d["frag"] / d["total"] * 100.0) if d["total"] else 0,
               d["median_dwell"], d["chats"], d["events"], d["event_min"]))
    return "".join(rows)


def trend_text(s):
    t = s["trend"]
    if not t:
        return "记录还太少，攒够两周才能比较「最近一周 vs 前一周」。"
    pct = t["pct"]
    if abs(pct) < 8:
        word = "基本持平"
        cls = "flat"
    elif pct > 0:
        word = "更频繁了 %.0f%%" % pct
        cls = "up"
    else:
        word = "缓和了 %.0f%%" % abs(pct)
        cls = "down"
    return ("最近 7 个工作日日均 <b>%.0f</b> 次，前 7 个工作日 <b>%.0f</b> 次 —— <span class='%s'>%s</span>。"
            % (t["recent"], t["before"], cls, word))


# ---------------------------------------------------------------- 页面

def render(days, stats, title):
    peak_day = stats["busiest"]
    frag_pct = stats["frag_pct"]
    html = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%(title)s</title>
<style>
  :root { --ink:#1c1f23; --muted:#767b82; --line:#e8eaed; --blue:#2a82da; --orange:#f0a24a; }
  * { box-sizing: border-box; }
  body { margin:0; padding:28px 24px 48px; background:#f6f7f9; color:var(--ink);
         font:14px/1.55 "Noto Sans CJK SC", "Source Han Sans SC", "Microsoft YaHei", system-ui, sans-serif; }
  .wrap { max-width: 1000px; margin: 0 auto; }
  h1 { font-size: 20px; margin: 0 0 4px; font-weight: 600; }
  .sub { color: var(--muted); font-size: 12px; margin-bottom: 20px; }
  .kpis { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; margin-bottom: 18px; }
  .kpi { background:#fff; border:1px solid var(--line); border-radius:10px; padding:12px 14px; }
  .kpi .k { color: var(--muted); font-size: 12px; }
  .kpi .v { font-size: 22px; font-weight: 600; margin-top: 2px; }
  .kpi .u { font-size: 12px; color: var(--muted); font-weight: 400; margin-left: 3px; }
  .card { background:#fff; border:1px solid var(--line); border-radius:10px; padding:16px 18px 10px; margin-bottom:16px; }
  .card h2 { font-size: 13px; font-weight: 600; margin: 0 0 2px; }
  .card .hint { color: var(--muted); font-size: 11px; margin-bottom: 10px; }
  .verdict { background:#fff; border:1px solid var(--line); border-left:3px solid var(--blue);
             border-radius:8px; padding:12px 16px; margin-bottom:16px; }
  .up { color:#c0392b; font-weight:600; } .down { color:#1e8449; font-weight:600; } .flat { color:#8b6d00; font-weight:600; }
  table { width:100%%; border-collapse: collapse; font-size: 12.5px; }
  th, td { padding: 6px 8px; border-bottom: 1px solid var(--line); text-align: left; }
  th { color: var(--muted); font-weight: 500; font-size: 11.5px; }
  td.num, th.num { text-align: right; font-variant-numeric: tabular-nums; }
  .legend { font-size: 11.5px; color: var(--muted); display:flex; gap:16px; margin-top: 6px; }
  .dot { display:inline-block; width:9px; height:9px; border-radius:2px; margin-right:5px; vertical-align:-1px; }
  footer { color: var(--muted); font-size: 11.5px; margin-top: 22px; }
  .scroll { max-height: 420px; overflow: auto; }
</style></head>
<body><div class="wrap">
<h1>%(title)s</h1>
<div class="sub">%(range)s · 共 %(days)d 天，其中 %(active)d 天有记录 · 数据来自本机 usage.db</div>

<div class="verdict">%(trend)s</div>

<div class="card" style="padding:12px 16px">
  <h2 style="margin-bottom:6px">统计口径</h2>
  <div class="hint" style="margin:0">「有效切换」= 原始记录里去掉这几类：① 上一屏停留不到 3 秒就切走（焦点/OCR 抖动）；② 间隔 20 分钟以上（午休、长会后回来）；③ 系统弹层（任务栏、控制中心、选择文件对话框、剪贴板）。原始的每条记录仍保留在数据库和导出 CSV 里，页面上两个数都给出，方便对照。<br>
  其余口径：碎片切换＝有效切换里上一屏停留不到 10 秒的；会话数＝当天切进去过的企业微信不同会话个数；外部事件＝手动记的会议/被叫走等；日均＝总有效切换 ÷ 有记录的天数。</div>
</div>

<div class="kpis">
  <div class="kpi"><div class="k">日均切换</div><div class="v">%(avg).0f<span class="u">次</span></div><div class="k">按有记录的 %(active)d 天平均</div></div>
  <div class="kpi"><div class="k">总切换</div><div class="v">%(total)d<span class="u">次</span></div><div class="k">原始记录 %(raw)d 条</div></div>
  <div class="kpi"><div class="k">碎片切换占比</div><div class="v">%(fragpct).0f<span class="u">%%</span></div><div class="k">停留 &lt; 10 秒</div></div>
  <div class="kpi"><div class="k">最忙一天</div><div class="v">%(peakday)s</div><div class="k">%(peakcount)d 次</div></div>
  <div class="kpi"><div class="k">会话数（单日最多）</div><div class="v">%(chats)d<span class="u">个</span></div></div>
  <div class="kpi"><div class="k">外部事件</div><div class="v">%(events)d<span class="u">次</span></div><div class="k">共 %(eventmin)d 分钟</div></div>
</div>

<div class="card">
  <h2>每天切换次数</h2>
  <div class="hint">蓝柱＝当天有效切换数，橙线＝7 天滑动平均（按自然日算，没记录的日子记 0；看趋势用这条）</div>
  %(line)s
  <div class="legend"><span><i class="dot" style="background:#2a82da"></i>每天有效切换</span>
  <span><i class="dot" style="background:#f0a24a"></i>7 天平均</span></div>
</div>

<div class="card">
  <h2>每天几点被打断</h2>
  <div class="hint">纵轴是小时，横轴是日期，颜色越深切换越多 —— 一眼看出你的「高危时段」是上午还是下午</div>
  %(heat)s
  <div class="legend"><span>少</span>
    <span><i class="dot" style="background:#2a82da;opacity:.15"></i><i class="dot" style="background:#2a82da;opacity:.45"></i><i class="dot" style="background:#2a82da;opacity:.75"></i><i class="dot" style="background:#2a82da;opacity:1"></i></span>
    <span>多（单小时最多 %(heatmax)d 次）</span></div>
</div>

<div class="card">
  <h2>逐日明细</h2>
  <div class="hint">停留中位数＝每次切换前在上一屏待了多久，越大越说明你坐得住</div>
  <div class="scroll"><table>
    <thead><tr><th>日期</th><th class="num">有效切换</th><th class="num">原始记录</th><th class="num">碎片</th><th class="num">碎片占比</th>
    <th class="num">停留中位数(s)</th><th class="num">会话数</th><th class="num">外部事件</th><th class="num">事件分钟</th></tr></thead>
    <tbody>%(rows)s</tbody>
  </table></div>
</div>

<footer>由 app-usage-monitor-report 生成 · 全部数据只在本机，页面不联网</footer>
</div></body></html>""" % {
        "title": title,
        "range": "%s ~ %s" % (days[0]["date"].strftime("%Y-%m-%d"), days[-1]["date"].strftime("%Y-%m-%d")),
        "days": stats["days"],
        "active": stats["active_days"],
        "trend": trend_text(stats),
        "avg": stats["avg"],
        "total": stats["total"],
        "raw": stats["raw"],
        "fragpct": frag_pct,
        "peakday": peak_day["date"].strftime("%m-%d") if peak_day else "-",
        "peakcount": peak_day["total"] if peak_day else 0,
        "chats": stats["chats"],
        "events": stats["events"],
        "eventmin": stats["event_min"],
        "line": line_chart(days),
        "heat": heatmap(days),
        "heatmax": max([max(d["hours"]) for d in days] + [0]),
        "rows": table(days),
    }
    return html


def main():
    ap = argparse.ArgumentParser(description="把上下文切换数据画成本地网页月报")
    ap.add_argument("--days", type=int, default=30)
    ap.add_argument("--from", dest="start")
    ap.add_argument("--to", dest="end")
    ap.add_argument("--db", default=DB_DEFAULT)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    if not os.path.exists(args.db):
        sys.exit("找不到数据库：%s" % args.db)

    today = dt.date.today()
    end = dt.date.fromisoformat(args.end) if args.end else today
    start = dt.date.fromisoformat(args.start) if args.start else end - dt.timedelta(days=args.days - 1)
    if start > end:
        sys.exit("--from 比 --to 还晚")

    days = load(args.db, start, end)
    stats = summarize(days)
    out = args.out or os.path.join(
        os.path.expanduser("~" + os.sep + "下载"), "app-usage-switch-report.html")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(render(days, stats, "上下文切换月报"))
    print("已生成：%s" % out)
    print("%s ~ %s：日均 %.0f 次，碎片占比 %.0f%%" % (start, end, stats["avg"], stats["frag_pct"]))
    if stats["trend"]:
        print("最近 7 天日均 %.0f 次，前 7 天 %.0f 次（%+.0f%%）"
              % (stats["trend"]["recent"], stats["trend"]["before"], stats["trend"]["pct"]))


if __name__ == "__main__":
    main()
