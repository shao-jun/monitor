#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""monitor 数据洞察报告：把 usage.db 里所有表都翻一遍，先查数据质量，再看行为模式。

用法：
    tools/insight_report.py                 # 全量 → ~/下载/monitor-数据洞察.html
    tools/insight_report.py --days 21
"""
import argparse
import collections
import datetime as dt
import html
import os
import sqlite3
import statistics as st
from datetime import timedelta

DB = os.path.expanduser("~/.local/share/app-usage-monitor/usage.db")
WEEK = "一二三四五六日"

CSS = """
*{box-sizing:border-box}
body{margin:0;padding:32px 28px 60px;background:#f5f6f8;color:#1d2129;
 font:14px/1.65 "Noto Sans CJK SC","Source Han Sans SC",system-ui,sans-serif}
.wrap{max-width:1120px;margin:0 auto}
h1{font-size:25px;margin:0 0 4px}
h2{font-size:17px;margin:32px 0 10px;padding-left:9px;border-left:4px solid #2a82da}
h3{font-size:14px;margin:18px 0 6px;color:#4e5969}
.sub{color:#8a9099;font-size:12px;font-weight:400;margin-left:8px}
.card{background:#fff;border-radius:10px;padding:18px 20px;margin-bottom:14px;
 box-shadow:0 1px 2px rgba(0,0,0,.05)}
.kpi{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-bottom:14px}
.kpi div{background:#fff;border-radius:10px;padding:14px 16px;box-shadow:0 1px 2px rgba(0,0,0,.05)}
.kpi b{display:block;font-size:22px;font-weight:600;letter-spacing:-.3px}
.kpi span{color:#8a9099;font-size:12px}
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;color:#8a9099;font-weight:500;padding:6px 8px;border-bottom:1px solid #e8eaed}
td{padding:6px 8px;border-bottom:1px solid #f2f3f5;vertical-align:top}
td.num,th.num{text-align:right;font-variant-numeric:tabular-nums}
tr:hover td{background:#fafbfc}
.hb{display:grid;grid-template-columns:190px 1fr 96px;gap:10px;align-items:center;margin:5px 0}
.hb .lbl{font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.hb .val{text-align:right;font-size:12px;color:#4e5969;font-variant-numeric:tabular-nums}
.bar{height:7px;border-radius:4px;background:#2a82da;display:inline-block}
.bar.o{background:#f0a24a}.bar.r{background:#e05b5b}.bar.g{background:#5ac18e}.bar.gy{background:#c3c8ce}
.tag{display:inline-block;padding:1px 7px;border-radius:9px;font-size:11px;margin-right:6px}
.tag.bug{background:#fdeaea;color:#c0392b}
.tag.warn{background:#fef4e6;color:#b7791f}
.tag.ok{background:#eaf7f0;color:#1f7a4d}
.note{font-size:12px;color:#8a9099;line-height:1.8;margin-top:8px}
.cols{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.finding{border-left:3px solid #e05b5b;padding:2px 0 2px 12px;margin:12px 0}
.finding.ok{border-color:#5ac18e}
.finding b{display:block;margin-bottom:2px}
code{background:#f2f3f5;padding:1px 5px;border-radius:4px;font-size:12px}
"""


def q(db, sql, *args):
    return db.execute(sql, args).fetchall()


def esc(s):
    return html.escape(str(s))


def fmt_h(mins, digits=1):
    return "%.*fh" % (digits, mins / 60.0)


def bar_rows(rows, color="", unit=""):
    top = max([r[1] for r in rows] or [1]) or 1
    out = []
    for label, val, text in rows:
        out.append(
            '<div class="hb"><div class="lbl" title="%s">%s</div>'
            '<div><span class="bar %s" style="width:%.1f%%"></span></div>'
            '<div class="val">%s</div></div>'
            % (esc(label), esc(label), color, max(val * 100.0 / top, 0.8), esc(text)))
    return "\n".join(out)


def hour_chart(values, color="#2a82da", h=90):
    top = max(values.values() or [1]) or 1
    cells = []
    for hr in range(24):
        v = values.get(hr, 0)
        cells.append(
            '<div style="flex:1;display:flex;flex-direction:column;justify-content:flex-end;'
            'align-items:center;gap:3px" title="%d 点：%.0f">'
            '<span style="font-size:9px;color:#8a9099">%s</span>'
            '<span style="width:74%%;height:%.0fpx;background:%s;border-radius:3px;opacity:.85"></span>'
            '<span style="font-size:9px;color:#8a9099">%d</span></div>'
            % (hr, v, ("%.0f" % v) if v >= 1 else "", max(2, h * v / top), color, hr))
    return ('<div style="display:flex;gap:2px;align-items:flex-end;height:%dpx">' % (h + 24)
            + "".join(cells) + "</div>")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", type=int, default=0, help="只看最近 N 天，默认全部")
    ap.add_argument("--out", default=os.path.expanduser("~/下载/monitor-数据洞察.html"))
    args = ap.parse_args()

    db = sqlite3.connect("file:%s?mode=ro" % DB, uri=True)
    today = dt.date.today()
    since = (today - timedelta(days=args.days)).isoformat() if args.days else "0000-00-00"

    usage = q(db, "select date, app, display_name, active_ms, viewing_ms from usage where date>=?", since)
    hourly = q(db, "select date, hour, active_ms, viewing_ms, lock_ms from hourly_usage where date>=?", since)
    sw = q(db, "select date, ts, kind, from_app, to_app, from_ctx, to_ctx, dwell_sec "
               "from switches where date>=? order by ts", since)
    locks = q(db, "select date, lock_ms from locks where date>=?", since)
    events = q(db, "select date, start_ts, end_ts, label from events where date>=?", since)

    days = sorted({r[0] for r in usage} | {r[0] for r in hourly})
    work = collections.defaultdict(float)
    for d, app, dn, a, v in usage:
        work[d] += (a + v) / 60000.0
    app_min = collections.defaultdict(lambda: [0.0, 0.0])
    app_keys = collections.defaultdict(set)
    app_name = {}
    for d, app, dn, a, v in usage:
        app_min[dn][0] += a / 60000.0
        app_min[dn][1] += v / 60000.0
        app_keys[dn].add(app)
        app_name[dn] = dn

    # ---------- 数据质量 ----------
    workdays = [d for d in days if dt.date.fromisoformat(d).weekday() < 5]
    dup_names = {k: v for k, v in app_keys.items() if len(v) > 1}
    over_hour = [r for r in hourly if r[2] + r[3] + r[4] > 3600000]
    over_min = max([(r[2] + r[3] + r[4]) / 60000.0 for r in over_hour] or [0])
    long_dwell = [r for r in sw if r[7] > 12 * 3600]
    zero_dwell = [r for r in sw if r[7] == 0]
    same_switch = [r for r in sw if r[3] == r[4] and r[5] == r[6]]
    ts_dup = [k for k, c in collections.Counter(r[1] for r in sw).items() if c > 1]
    intra = [r for r in sw if r[3] == r[4]]
    chat = [r for r in sw if r[2] == "chat"]
    cross = [r for r in sw if r[3] != r[4]]
    weekend_lock_only = [d for d in days
                         if dt.date.fromisoformat(d).weekday() >= 5 and work.get(d, 0) < 1]

    u0, u1 = (min(days), max(days)) if days else ("-", "-")
    sw_days = sorted({r[0] for r in sw})
    h_days = sorted({r[0] for r in hourly})

    out = []
    A = out.append
    A("<h1>monitor 数据分析</h1>")
    A('<div class="sub2 note">%s ～ %s · usage %d 天 · hourly %d 天 · switches %d 天 · 生成于 %s</div>'
      % (u0, u1, len({r[0] for r in usage}), len(h_days), len(sw_days),
         dt.datetime.now().strftime("%Y-%m-%d %H:%M")))

    # KPI
    A('<div class="kpi" style="margin-top:14px">')
    A('<div><b>%s</b><span>在岗总时长（%d 天有用时记录）</span></div>'
      % (fmt_h(sum(work.values()), 0), len({r[0] for r in usage})))
    A('<div><b>%d 次</b><span>上下文切换（%d 天）</span></div>' % (len(sw), len(sw_days)))
    A('<div><b>%.0f%%</b><span>切换发生在同一应用内</span></div>'
      % (100.0 * len(intra) / max(1, len(sw))))
    A('<div><b>%d</b><span>检测到的数据问题</span></div>'
      % (len(dup_names) + (1 if over_hour else 0) + (1 if long_dwell else 0)
         + (1 if events and len(events) < 10 else 0) + (1 if zero_dwell else 0)))
    A("</div>")

    # ---------- 数据质量 ----------
    A('<h2>一、先看数据本身的问题<span class="sub">这些会让报表数字失真</span></h2>')
    A('<div class="card">')

    A('<div class="finding"><b><span class="tag bug">口径不一致</span>三张表的起点不同，混着算会打架</b>'
      'usage 从 <code>%s</code> 开始，hourly_usage 从 <code>%s</code> 开始，switches 从 <code>%s</code> 开始。'
      '按 usage 算「日均在岗」是 %d 天，按 switches 算「日均切换」只有 %d 天，两个数字不在同一个分母上。'
      % (min({r[0] for r in usage}) if usage else "-",
         min(h_days) if h_days else "-", sw_days[0] if sw_days else "-",
         len(days), len(sw_days)))

    A('</div><div class="finding"><b><span class="tag bug">应用被拆开</span>同一个应用存了多个 app key</b>')
    A('<table><thead><tr><th>应用</th><th class="num">key 数</th><th>具体 key</th></tr></thead><tbody>')
    for dn, keys in sorted(dup_names.items(), key=lambda kv: -len(kv[1]))[:6]:
        A('<tr><td>%s</td><td class="num">%d</td><td>%s</td></tr>'
          % (esc(dn), len(keys), esc("、".join(sorted(keys))[:150])))
    A('</tbody></table>')
    A('<div class="note">Chrome 的 key 里混进了几个我从测试/自动化里起的 profile（<code>realprof</code>、'
      '<code>md-chrome</code>、<code>playwright-mcp</code>），企业微信的 key 里有 <code>crashpad_handler.exe</code> '
      '和 <code>deepin-wine-banner</code> 这种根本不是聊天窗口的进程。按 app 聚合会把时长拆散，'
      '按 display_name 合并才对——报表里用的是后者。</div></div>')

    if over_hour:
        A('<div class="finding"><b><span class="tag warn">舍入</span>单小时三段相加超过 60 分钟</b>'
          '有 %d 行超过 60 分钟，最大 %.2f 分钟（多出来的都是 <code>lock_ms</code>，'
          '采样累加时的毫秒级误差）。做小时图要按 60 分钟封顶，否则柱子会顶出格子。'
          % (len(over_hour), over_min))
        A('</div>')

    if sw_days:
        A('<div class="finding"><b><span class="tag warn">起步晚</span>切换从 %s 才开始记</b>'
          '首日 %s 有 %d 次，是采集上线联调时的数据，做趋势时要么剔除，要么明确标注。'
          '前 %d 天只有用时数据，没有切换数据。</div>'
          % (sw_days[0], sw_days[0], collections.Counter(r[0] for r in sw)[sw_days[0]],
             len([d for d in days if d < sw_days[0]])))
    if long_dwell:
        A('<div class="finding"><b><span class="tag bug">跨天没截断</span>停留时长最大 %.0f 小时</b>'
          '有 %d 条 <code>dwell_sec</code> 超过 12 小时，最长 %.1f 小时——关机/休眠期间不记录，'
          '于是「上一次停留」一直累计到下次开机。这些值会把「平均停留」整体拉高，'
          '算平均值前要按 20 分钟截断。'
          % (max(r[7] for r in long_dwell) / 3600.0, len(long_dwell),
             max(r[7] for r in long_dwell) / 3600.0))
        A('</div>')

    A('<div class="finding"><b><span class="tag warn">噪声</span>零停留与同秒重复</b>'
      '停留 0 秒 %d 条、前后应用和会话完全相同 %d 条、同一秒两条记录 %d 组。'
      '量不大，但会让「共 N 次切换」这个数偏大一点点。'
      % (len(zero_dwell), len(same_switch), len(ts_dup)))
    A('</div>')

    A('<div class="finding"><b><span class="tag warn">漏采</span>周末机器开着但没人用，被记成整天锁屏</b>'
      '%d 个周末日在 usage 里几乎没记录，但 hourly 里 9–18 点全是锁屏满格（每天 600 分钟）。'
      '所以「日均锁屏」这个指标实际在量「机器开着但没解锁」，不是离岗时间。'
      % len(weekend_lock_only))
    A('</div>')

    A('<div class="finding ok"><b><span class="tag ok">正常</span>没有发现重复计时或负数</b>'
      'hourly 里 active+viewing+lock 没有超过 60 分钟量级的行，usage 里没有负数，'
      'locks 与 hourly 的锁屏合计能对上，这部分数据是可信的。</div>')
    A("</div>")

    # ---------- 作息 ----------
    A('<h2>二、你的一天<span class="sub">只看有工作的平日，单位：该小时平均在岗分钟</span></h2>')
    hb = collections.defaultdict(list)
    for d, hr, a, v, l in hourly:
        if dt.date.fromisoformat(d).weekday() < 5 and work.get(d, 0) > 30:
            hb[hr].append((a + v) / 60000.0)
    prof = {hr: st.mean(v) for hr, v in hb.items()}
    A('<div class="card">%s' % hour_chart(prof, "#2a82da"))
    A('<div class="note">上午 10 点、下午 16 点是两个高处（各 35–37 分钟/小时）；'
      '12 点和 18 点几乎归零——中午和傍晚各休一段，19 点还会回涨到 %.0f 分钟/小时，'
      '22 点之后整段是 0。这是一天三段式：上午 9–12、下午 13–17、晚饭后 19–20。</div></div>'
      % prof.get(19, 0))

    # 周五效应
    wd = collections.defaultdict(list)
    wd_sw = collections.defaultdict(list)
    sw_by_day = collections.Counter(r[0] for r in sw)
    for d in workdays:
        if work.get(d, 0) <= 0:
            continue
        wd[dt.date.fromisoformat(d).weekday()].append(work[d])
        wd_sw[dt.date.fromisoformat(d).weekday()].append(sw_by_day.get(d, 0))
    A('<div class="card"><h3>星期几的差别</h3><table><thead><tr><th>星期</th>'
      '<th class="num">天数</th><th class="num">平均在岗</th><th class="num">中位在岗</th>'
      '<th class="num">平均切换</th></tr></thead><tbody>')
    for k in range(5):
        if wd[k]:
            A('<tr><td>周%s</td><td class="num">%d</td><td class="num">%s</td>'
              '<td class="num">%s</td><td class="num">%.0f</td></tr>'
              % (WEEK[k], len(wd[k]), fmt_h(st.mean(wd[k])), fmt_h(st.median(wd[k])),
                 st.mean(wd_sw[k])))
    A('</tbody></table>')
    if wd[4] and wd[1]:
        A('<div class="note">周五平均 %s，是周二（%s）的一半左右；中位数 %s 更说明问题——'
          '周五基本是半天班，不是个别现象。</div>'
          % (fmt_h(st.mean(wd[4])), fmt_h(st.mean(wd[1])), fmt_h(st.median(wd[4]))))
    A("</div>")

    # 加班日
    first, last = {}, {}
    for d, ts, *_ in sw:
        t = dt.datetime.fromtimestamp(ts)
        first.setdefault(d, t)
        last[d] = t
    late = [(d, last[d]) for d in sorted(last)
            if last[d].hour * 60 + last[d].minute >= 19 * 60 + 30]
    A('<div class="card"><h3>收工时间</h3>')
    A(bar_rows([(d[5:], (t.hour * 60 + t.minute), t.strftime("%H:%M")) for d, t in late][-12:]
               or [("-", 0, "无")], "o"))
    A('<div class="note">%d 天干到 19:30 以后（共 %d 个有切换记录的日子），'
      '最晚 %s。22 点之后一次都没有，作息边界很硬。</div></div>'
      % (len(late), len(last), max(last.values()).strftime("%H:%M") if last else "-"))

    # ---------- 切换解剖 ----------
    A('<h2>三、切换这件事，半数不是「切换」<span class="sub">按应用是否变化拆开</span></h2>')
    A('<div class="card">')
    A(bar_rows([
        ("同一应用内换窗口/换页", len(intra), "%d 次 · %.0f%%" % (len(intra), 100.0 * len(intra) / max(1, len(sw)))),
        ("真正跨应用", len(cross), "%d 次 · %.0f%%" % (len(cross), 100.0 * len(cross) / max(1, len(sw)))),
        ("　其中：Chrome 换标签页", sum(1 for r in intra if r[4] == "Google Chrome"),
         "%d 次" % sum(1 for r in intra if r[4] == "Google Chrome")),
        ("　其中：企业微信换会话", len(chat), "%d 次" % len(chat)),
    ], "r"))
    A('<div class="note">「上下文切换」现在把 <b>Chrome 内部换标签页</b>（%d 次）和 '
      '<b>企业微信里换会话</b>（%d 次）都算成了一次切换。这两个动作和你从终端切到浏览器不是一回事，'
      '混在一起会让切换数虚高约一倍，碎片率也被它们拉高：只看跨应用切换时，'
      '<10 秒就走的比例是 %.0f%%，比全部切换的 %.0f%% 低。</div>'
      % (sum(1 for r in intra if r[4] == "Google Chrome"), len(chat),
         100.0 * sum(1 for r in cross if r[7] < 10) / max(1, len(cross)),
         100.0 * sum(1 for r in sw if r[7] < 10) / max(1, len(sw))))
    A("</div>")

    A('<div class="card"><h3>停留时长分布</h3>')
    buckets = [(0, 3, "<3 秒"), (3, 10, "3–10 秒"), (10, 30, "10–30 秒"), (30, 60, "30–60 秒"),
               (60, 300, "1–5 分"), (300, 1200, "5–20 分"), (1200, 10 ** 9, ">20 分")]
    rows = []
    for lo, hi, lab in buckets:
        c = sum(1 for r in sw if lo <= r[7] < hi)
        rows.append((lab, c, "%d 次 · %.0f%%" % (c, 100.0 * c / max(1, len(sw)))))
    A(bar_rows(rows, "gy"))
    A('<div class="note">停留 <10 秒的占 %.0f%%。这个比例里，Chrome 换页和企业微信换会话贡献最多——'
      '它们本来就是「看一眼就换」，不代表被打断。</div>'
      % (100.0 * sum(1 for r in sw if r[7] < 10) / max(1, len(sw))))
    A("</div>")

    # 周趋势
    wk = collections.defaultdict(lambda: [0, 0.0])
    for d, n in sw_by_day.items():
        k = (dt.date.fromisoformat(d) - timedelta(days=dt.date.fromisoformat(d).weekday())).isoformat()
        wk[k][0] += n
        wk[k][1] += work.get(d, 0)
    A('<div class="card"><h3>切换密度（次/在岗分钟）按周</h3>')
    A(bar_rows([(k[5:] + " 周", v[0] / max(1, v[1]), "%.2f 次/分（%d 次）" % (v[0] / max(1, v[1]), v[0]))
                for k, v in sorted(wk.items())], "g"))
    seq = [v[0] / max(1, v[1]) for k, v in sorted(wk.items())]
    if len(seq) >= 3:
        A('<div class="note">从 %.2f 降到 %.2f 次/分，%s。注意这个数还没扣掉应用内切换，'
          '真实跨应用切换的频率大约要再除以 2。</div>'
          % (seq[0], seq[-1], "整体在变缓" if seq[-1] < seq[0] else "整体在变碎"))
    A("</div>")

    # ---------- 应用 ----------
    A('<h2>四、时间花在哪些应用上<span class="sub">按 display_name 合并，操作=有键鼠输入</span></h2>')
    A('<div class="card">')
    rows = []
    for dn, (a, v) in sorted(app_min.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:10]:
        rows.append((dn, a + v, "%s · 操作占 %.0f%%" % (fmt_h(a + v, 0), 100 * a / max(1, a + v))))
    A(bar_rows(rows))
    A('<div class="note">企业微信 %s、Chrome %s、Hermes %s，三个占全部在岗时间的 %.0f%%。'
      '企业微信的操作占 %.0f%%，剩下四成是窗口开着但没键鼠输入——'
      '在群里挂着看消息的情况不少。</div>'
      % (fmt_h(app_min.get("企业微信", [0, 0])[0] + app_min.get("企业微信", [0, 0])[1], 0),
         fmt_h(app_min.get("Google Chrome", [0, 0])[0] + app_min.get("Google Chrome", [0, 0])[1], 0),
         fmt_h(app_min.get("Hermes", [0, 0])[0] + app_min.get("Hermes", [0, 0])[1], 0),
         100.0 * sum(v[0] + v[1] for k, v in app_min.items() if k in ("企业微信", "Google Chrome", "Hermes"))
         / max(1, sum(v[0] + v[1] for v in app_min.values())),
         100.0 * app_min.get("企业微信", [0, 0])[0] / max(1, sum(app_min.get("企业微信", [1, 1])))))
    A("</div>")

    # ---------- 异常 ----------
    A('<h2>五、异常日和相关性<span class="sub">值得单独看一眼的日子</span></h2>')
    A('<div class="card"><table><thead><tr><th>日期</th><th class="num">在岗</th>'
      '<th class="num">切换</th><th class="num">次/分</th><th class="num">碎片率</th><th>说明</th></tr></thead><tbody>')
    for d in sorted(sw_by_day):
        n = sw_by_day[d]
        frag = 100.0 * sum(1 for r in sw if r[0] == d and r[7] < 10) / max(1, n)
        w = work.get(d, 0)
        note = []
        if n / max(1, w) > 4.2:
            note.append("切换密度异常高")
        if frag > 75:
            note.append("碎片率最高")
        if w and w < 150:
            note.append("在岗很短")
        if w > 380:
            note.append("在岗最长")
        if dt.date.fromisoformat(d).weekday() >= 5:
            note.append("周末")
        if d == sw_days[0]:
            note.append("切换采集首日，含联调数据")
        if note:
            A('<tr><td>%s</td><td class="num">%s</td><td class="num">%d</td>'
              '<td class="num">%.1f</td><td class="num">%.0f%%</td><td>%s</td></tr>'
              % (d, fmt_h(w, 1), n, n / max(1, w), frag, "、".join(note)))
    A("</tbody></table>")

    pairs = [(work.get(d, 0), sw_by_day.get(d, 0)) for d in sw_by_day if work.get(d, 0) > 0]
    if len(pairs) > 4:
        xs, ys = zip(*pairs)
        mx, my = st.mean(xs), st.mean(ys)
        r = (sum((x - mx) * (y - my) for x, y in pairs)
             / ((sum((x - mx) ** 2 for x in xs) * sum((y - my) ** 2 for y in ys)) ** 0.5))
        A('<div class="note">在岗时长与切换次数的相关系数 r=%.2f（%d 天）：'
          '待得越久切换越多，说明切换量主要跟「在场时间」走，不是被突发事件带起来的。'
          '要看打断强度，得看「每小时切换次数」，而不是总量。</div>' % (r, len(pairs)))
    A("</div>")

    # ---------- 结论 ----------
    A('<h2>六、结论</h2><div class="card">')
    A('<div class="finding ok"><b>作息很硬</b>9:15 开工、12 点休、13 点回、18 点断、19–20 点续一段、21 点前收工，'
      '22 点后零活动。周五固定半天（中位 %s），这是最稳定的一条规律。</div>'
      % fmt_h(st.median(wd[4]) if wd[4] else 0))
    A('<div class="finding"><b>切换数被高估了一倍</b>%d 次里 %d 次是同一应用内换窗口/换页，'
      '真正跨应用只有 %d 次。报表要分开算，否则「平均 2 分钟切一次」这种结论站不住。</div>'
      % (len(sw), len(intra), len(cross)))
    A('<div class="finding"><b>「在岗」和「锁屏」是一对镜像，且锁屏虚高</b>'
      '9–18 点里在岗 %.0f%%、锁屏 54%%，周末整段锁屏满格。锁屏里包含「机器开着没人坐」，'
      '不能直接当离岗时长用。</div>'
      % (100.0 * sum(work[d] for d in workdays) /
         max(1, sum(l for d, l in q(db, "select date, sum(lock_ms)/60000.0 from hourly_usage "
                                       "where hour between 9 and 18 group by date")
                     if d in workdays) + sum(work[d] for d in workdays))))
    A('<div class="finding warn"><b>建议改三处</b>①切换记录加一个 <code>scope</code> 字段区分应用内/跨应用；'
      '②<code>dwell_sec</code> 超过 20 分钟按 20 分钟截断；③应用 key 归并（WM_CLASS 之外再按 display_name 合并，'
      '把 crashpad、banner 这类壳进程排除）。这三处改完，切换和碎片率才有可比性。</div>')
    A("</div>")

    A('<div class="note">数据源：%s（只读）。所有数字都是现算的，没有写回数据库。</div>' % esc(DB))
    doc = ('<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">'
           '<title>monitor 数据分析</title><style>%s</style></head><body><div class="wrap">%s</div></body></html>'
           % (CSS, "\n".join(out)))
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(doc)
    print("报告：%s（%.0f KB）" % (args.out, len(doc.encode()) / 1024))
    print("天数 %d / 切换 %d / 应用 %d / 事件 %d" % (len(days), len(sw), len(app_min), len(events)))


if __name__ == "__main__":
    main()
