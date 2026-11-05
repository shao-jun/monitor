#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""工作内容报告：企业微信（跟谁聊 / 聊什么）+ Chrome（上哪些网站）+ 任务栏切换数据

数据源（全部本地、只读）：
  1. 企业微信 message.db —— 经 wecom-agent 解密后读取（复用 wxwork_crypto / wal_snapshot / export_wxwork）
  2. Chrome History —— 复制一份再读（Chrome 运行时库被锁）
  3. app-usage-monitor usage.db —— 会话切换次数、应用时长

用法：
    app-usage-monitor-work-report --days 30
    app-usage-monitor-work-report --days 7 --out /tmp/week.html
"""
import argparse
import html
import json
import os
import re
import shutil
import sqlite3
import sys
import time
import tempfile
from collections import Counter, defaultdict
from datetime import datetime, timedelta
from urllib.parse import urlparse

USAGE_DB = os.path.expanduser("~/.local/share/app-usage-monitor/usage.db")
WECOM_AGENT = os.environ.get("WECOM_AGENT_DIR",
                             os.path.expanduser("~/src/wecom-agent/decrypt"))
CHROME_DIRS = [
    os.path.expanduser("~/.config/google-chrome"),
    os.path.expanduser("~/.config/chromium"),
    os.path.expanduser("~/.config/microsoft-edge"),
]

# 域名 → 好认的名字（报告里不出现内网地址，只写用途）
SITE_NAMES = {
    "gerrit.uniontech.com": "Gerrit 代码评审",
    "itsm.uniontech.com": "ITSM 服务工单",
    "cooperation.uniontech.com": "明道云协作",
    "metabase.uniontech.com": "Metabase 数据看板",
    "pms.uniontech.com": "PMS 禅道",
    "wiki.uniontech.com": "内部 Wiki",
    "github.com": "GitHub",
    "gitlab.com": "GitLab",
    "stackoverflow.com": "Stack Overflow",
    "developer.mozilla.org": "MDN 文档",
    "doc.qt.io": "Qt 文档",
    "wiki.archlinux.org": "Arch Wiki",
    "debian.org": "Debian",
    "packages.debian.org": "Debian 软件包",
    "bugs.debian.org": "Debian Bug",
    "google.com": "Google 搜索",
    "www.google.com": "Google 搜索",
    "cn.bing.com": "Bing 搜索",
    "bing.com": "Bing 搜索",
    "baidu.com": "百度",
    "www.baidu.com": "百度",
    "zhihu.com": "知乎",
    "www.zhihu.com": "知乎",
    "juejin.cn": "掘金",
    "blog.csdn.net": "CSDN",
    "csdn.net": "CSDN",
    "gitee.com": "Gitee",
    "openai.com": "OpenAI",
    "chat.openai.com": "ChatGPT",
    "chatgpt.com": "ChatGPT",
    "claude.ai": "Claude",
    "kimi.moonshot.cn": "Kimi",
    "yuanbao.tencent.com": "腾讯元宝",
    "npmmirror.com": "npmmirror 镜像",
    "registry.npmmirror.com": "npmmirror 镜像",
    "pypi.org": "PyPI",
    "mirrors.aliyun.com": "阿里云镜像",
    "mirrors.tuna.tsinghua.edu.cn": "清华镜像",
    "bilibili.com": "哔哩哔哩",
    "www.bilibili.com": "哔哩哔哩",
    "youtube.com": "YouTube",
    "www.youtube.com": "YouTube",
    "weixin.qq.com": "微信公众平台",
    "mp.weixin.qq.com": "微信公众号文章",
}

TOPICS = [
    ("需求/方案", ("需求", "方案", "设计", "评审", "规划", "立项", "招标", "投标", "拆解")),
    ("版本/发布", ("版本", "发布", "合入", "编译", "打包", "deb", "回归", "灰度", "升级", "更新",
                   "补丁", "patch", "tag", "分支")),
    ("故障/排查", ("崩溃", "故障", "报错", "宕机", "卡死", "异常", "复现", "日志", "core", "crash",
                   "闪退", "黑屏", "花屏", "无法启动", "起不来", "卡顿", "死机")),
    ("工单/客户", ("工单", "ztgd", "客户", "现场", "反馈", "投诉", "报修", "支持", "售后", "银行",
                   "电网", "分行", "验收")),
    ("测试/验证", ("测试", "验证", "用例", "自测", "复测", "qa", "unstable", "通过率")),
    ("会议/沟通", ("会议", "开会", "周会", "例会", "评审会", "对齐", "沟通", "讨论", "汇报", "同步",
                   "邀请你参加")),
    ("文档/材料", ("文档", "材料", "ppt", "表格", "总结", "周报", "月报", "记录", "附件")),
    ("排期/进度", ("排期", "进度", "计划", "节点", "延期", "截止", "deadline", "交付", "工作量")),
    ("人事/行政", ("请假", "报销", "打卡", "考勤", "入职", "离职", "调休", "加班", "工牌")),
    ("项目/交付", ("项目", "交付", "试点", "上线", "投产", "演练", "定制")),
]


def classify(text, conversation=""):
    low = (text or "").casefold()
    for name, words in TOPICS:
        if any(w in low for w in words):
            return name
    # 消息本身看不出来就看它在哪个会话里：工单群/项目群本身就有明确主题
    conv_low = (conversation or "").casefold()
    for name, words in TOPICS:
        if any(w in conv_low for w in words):
            return name
    if len((text or "").strip()) <= 8:
        return "短回复/闲聊"
    return "其他"


def fmt_dur(seconds):
    seconds = int(seconds)
    h, m = divmod(seconds // 60, 60)
    if h >= 24:
        d, h = divmod(h, 24)
        return "%d天%d时" % (d, h)
    return "%d时%02d分" % (h, m) if h else "%d分" % m


def esc(s):
    return html.escape(str(s or ""))


# ---------------------------------------------------------------- 企业微信

def load_wecom(days):
    if not os.path.isdir(WECOM_AGENT):
        return None, "未找到 wecom-agent（%s）" % WECOM_AGENT
    sys.path.insert(0, os.path.join(WECOM_AGENT, "linux"))
    sys.path.insert(0, WECOM_AGENT)
    try:
        import wecom_linux as wl
    except ImportError as err:
        return None, "导入 wecom_linux 失败：%s" % err
    try:
        store = wl.open_store(wl.get_key())
    except SystemExit as err:
        return None, str(err) or "读取企业微信失败"
    users, convs, self_uid = wl.load_names(wl.get_key())
    me = wl.res_sender(self_uid, users) if self_uid else "我"

    people = defaultdict(lambda: {"n": 0, "mine": 0, "theirs": 0, "days": set(),
                                  "last": None, "topics": Counter(), "conv_id": ""})
    convs_agg = defaultdict(lambda: {"n": 0, "mine": 0, "theirs": 0, "days": set(),
                                     "first": None, "last": None, "topics": Counter(),
                                     "senders": Counter()})
    topic_agg = defaultdict(lambda: {"n": 0, "convs": Counter(), "samples": []})
    hours = Counter()
    per_day = Counter()
    total = mine_n = 0
    for row in store.iter_messages(days=days):
        conv, sender, text = row["conversation"], row["sender"], row["content"]
        ts = wl._norm_ts(row["send_time"])
        topic = classify(text, conv)
        conv_id = row.get("conv_id", "")
        person = users.get(str(conv_id))
        total += 1
        per_day[datetime.fromtimestamp(ts).date()] += 1
        hours[datetime.fromtimestamp(ts).hour] += 1
        rec = convs_agg[conv]
        if person:
            rec["single"] = True
        rec["n"] += 1
        rec["senders"][sender] += 1
        rec["topics"][topic] += 1
        rec["days"].add(datetime.fromtimestamp(ts).date())
        rec["first"] = ts if rec["first"] is None else min(rec["first"], ts)
        rec["last"] = ts if rec["last"] is None else max(rec["last"], ts)
        if sender == me:
            rec["mine"] += 1
            mine_n += 1
        else:
            rec["theirs"] += 1
        if person:
            pr = people[person]
            pr["n"] += 1
            pr["days"].add(datetime.fromtimestamp(ts).date())
            pr["last"] = ts if pr["last"] is None else max(pr["last"], ts)
            pr["topics"][topic] += 1
            pr["conv_id"] = str(conv_id)
            if sender == me:
                pr["mine"] += 1
            else:
                pr["theirs"] += 1
        tr = topic_agg[topic]
        tr["n"] += 1
        tr["convs"][conv] += 1
        if topic != "其他" and len(tr["samples"]) < 40 and 12 <= len(text or "") <= 220:
            tr["samples"].append((datetime.fromtimestamp(ts).strftime("%m-%d %H:%M"),
                                  conv, sender, text.replace("\n", " ")))

    people_rows = []
    for name, rec in people.items():
        people_rows.append({
            "name": name, "n": rec["n"], "mine": rec["mine"], "theirs": rec["theirs"],
            "active_days": len(rec["days"]),
            "last": datetime.fromtimestamp(rec["last"]).strftime("%Y-%m-%d %H:%M"),
            "topic": rec["topics"].most_common(1)[0][0] if rec["topics"] else "其他",
        })
    people_rows.sort(key=lambda r: -r["n"])

    conv_rows = []
    for name, rec in convs_agg.items():
        conv_rows.append({
            "name": name, "n": rec["n"], "mine": rec["mine"], "theirs": rec["theirs"],
            "active_days": len(rec["days"]),
            "last": datetime.fromtimestamp(rec["last"]).strftime("%Y-%m-%d %H:%M"),
            "first": datetime.fromtimestamp(rec["first"]).strftime("%Y-%m-%d"),
            "topic": rec["topics"].most_common(1)[0][0] if rec["topics"] else "其他",
            "senders": rec["senders"].most_common(3),
            "single": rec.get("single", False),
        })
    conv_rows.sort(key=lambda r: -r["n"])
    topic_rows = [{"topic": k, "n": v["n"], "convs": v["convs"].most_common(4),
                   "samples": v["samples"][:3]}
                  for k, v in sorted(topic_agg.items(), key=lambda kv: -kv[1]["n"])]
    return {
        "total": total, "mine": mine_n, "conversations": len(conv_rows),
        "people": people_rows,
        "conv_rows": conv_rows, "topic_rows": topic_rows,
        "hours": hours, "per_day": per_day, "me": me,
    }, None


# ---------------------------------------------------------------- Chrome

def chrome_db():
    for root in CHROME_DIRS:
        for prof in ("Default", "Profile 1", "Profile 2"):
            path = os.path.join(root, prof, "History")
            if os.path.exists(path) and os.path.getsize(path) > 4096:
                return path
    return None


def load_chrome(since):
    src = chrome_db()
    if not src:
        return None, "没找到 Chrome 历史库"
    tmpdir = tempfile.mkdtemp(prefix="chrome-hist-")
    dst = os.path.join(tmpdir, "History")
    shutil.copy2(src, dst)
    for suffix in ("-wal", "-shm"):
        if os.path.exists(src + suffix):
            shutil.copy2(src + suffix, dst + suffix)
    conn = sqlite3.connect("file:%s?mode=ro" % dst, uri=True)
    conn.text_factory = lambda b: b.decode("utf-8", "replace") if isinstance(b, bytes) else b
    chrome_epoch = 11644473600
    rows = conn.execute(
        "SELECT u.url, u.title, v.visit_time FROM visits v JOIN urls u ON u.id = v.url "
        "ORDER BY v.visit_time").fetchall()
    conn.close()
    shutil.rmtree(tmpdir, ignore_errors=True)

    visits = []
    for url, title, vt in rows:
        ts = vt / 1000000.0 - chrome_epoch
        if ts < since:
            continue
        visits.append((ts, url, title or ""))
    visits.sort()

    site_time = Counter()
    site_visits = Counter()
    site_titles = defaultdict(Counter)
    page_visits = Counter()
    page_title = {}
    page_time = Counter()
    hours = Counter()
    per_day = Counter()
    for i, (ts, url, title) in enumerate(visits):
        host = (urlparse(url).hostname or "").lower()
        site = site_of(host)
        hours[datetime.fromtimestamp(ts).hour] += 1
        per_day[datetime.fromtimestamp(ts).date()] += 1
        site_visits[site] += 1
        page_visits[url] += 1
        page_title.setdefault(url, title)
        # 停留估计：到下一次访问的间隔，同站点封顶 5 分钟，跨站点封顶 2 分钟
        if i + 1 < len(visits):
            delta = visits[i + 1][0] - ts
            nxt = site_of((urlparse(visits[i + 1][1]).hostname or "").lower())
            cap = 300 if nxt == site else 120
            dwell = min(max(delta, 0), cap)
        else:
            dwell = 0
        site_time[site] += dwell
        page_time[url] += dwell
        if title:
            site_titles[site][title] += 1
    if not visits:
        return None, "Chrome 历史里没有最近 %d 天的访问记录" % max(1, (time.time() - since) // 86400)
    days_covered = sorted({datetime.fromtimestamp(v[0]).date() for v in visits})
    return {
        "first_day": days_covered[0], "last_day": days_covered[-1],
        "covered_days": len(days_covered),
        "visits": len(visits), "sites": len(site_visits), "pages": len(page_visits),
        "site_visits": site_visits, "site_time": site_time,
        "site_titles": site_titles, "page_visits": page_visits,
        "page_title": page_title, "page_time": page_time,
        "hours": hours, "per_day": per_day, "source": src,
    }, None


WORK_SITES = ("Gerrit 代码评审", "ITSM 服务工单", "明道云协作", "Metabase 数据看板",
              "PMS 禅道", "内部 Wiki", "GitHub", "GitLab", "Gitee", "Debian",
              "Debian 软件包", "Debian Bug", "npmmirror 镜像", "PyPI", "阿里云镜像",
              "清华镜像", "内部服务", "shuttle", "研发自动化管理平台")
REF_SITES = ("MDN 文档", "Qt 文档", "Arch Wiki", "Stack Overflow", "Google 搜索",
             "Bing 搜索", "百度", "CSDN", "知乎", "掘金", "微信公众号文章")
AI_SITES = ("OpenAI", "ChatGPT", "Claude", "Kimi", "腾讯元宝")


def site_kind(site):
    if site in WORK_SITES:
        return "工作系统"
    if site in AI_SITES:
        return "AI 助手"
    if site in REF_SITES:
        return "查资料"
    if site in ("哔哩哔哩", "YouTube"):
        return "视频"
    return "其他"


def site_of(host):
    if not host:
        return "其他"
    for key, name in SITE_NAMES.items():
        if host == key or host.endswith("." + key):
            return name
    if re.match(r"^[\d.]+$", host):
        return "内网服务"
    parts = host.split(".")
    return ".".join(parts[-2:]) if len(parts) >= 2 else host


# ---------------------------------------------------------------- 应用侧

def load_usage(days):
    if not os.path.exists(USAGE_DB):
        return None
    conn = sqlite3.connect("file:%s?mode=ro" % USAGE_DB, uri=True)
    since = (datetime.now() - timedelta(days=days)).strftime("%Y-%m-%d")
    chat = conn.execute(
        "SELECT to_ctx, COUNT(*) FROM switches WHERE date >= ? AND kind='chat' "
        "GROUP BY to_ctx ORDER BY 2 DESC LIMIT 40", (since,)).fetchall()
    apps = conn.execute(
        "SELECT app, SUM(active_ms + viewing_ms) FROM usage WHERE date >= ? "
        "GROUP BY app ORDER BY 2 DESC LIMIT 12", (since,)).fetchall()
    conn.close()
    return {"chat": chat, "apps": apps}


# ---------------------------------------------------------------- 渲染

CSS = """
*{box-sizing:border-box}
body{margin:0;padding:32px 28px 60px;background:#f5f6f8;color:#1d2129;
  font:14px/1.6 "Noto Sans CJK SC","Source Han Sans SC",system-ui,sans-serif}
.wrap{max-width:1180px;margin:0 auto}
h1{font-size:25px;margin:0 0 6px}
h2{font-size:17px;margin:34px 0 12px;padding-left:9px;border-left:4px solid #2a82da}
h2 .sub{font-weight:400;font-size:12px;color:#8a9099;margin-left:8px}
.sub2{color:#8a9099;font-size:12px}
.card{background:#fff;border-radius:10px;padding:18px 20px;margin-bottom:14px;
  box-shadow:0 1px 2px rgba(0,0,0,.05)}
.kpi{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}
.kpi div{background:#fff;border-radius:10px;padding:14px 16px;box-shadow:0 1px 2px rgba(0,0,0,.05)}
.kpi b{display:block;font-size:22px;font-weight:600;letter-spacing:-.3px}
.kpi span{color:#8a9099;font-size:12px}
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;color:#8a9099;font-weight:500;padding:6px 8px;border-bottom:1px solid #e8eaed;
  position:sticky;top:0;background:#fff}
td{padding:6px 8px;border-bottom:1px solid #f2f3f5;white-space:nowrap;overflow:hidden;
  text-overflow:ellipsis;max-width:330px}
tr:hover td{background:#fafbfc}
.num{text-align:right;font-variant-numeric:tabular-nums}
.bar{height:7px;border-radius:4px;background:#2a82da;display:inline-block;vertical-align:middle}
.bar.o{background:#f0a24a}.bar.g{background:#5ac18e}.bar.p{background:#9b8cf0}.bar.gray{background:#c3c8ce}
.hb{display:grid;grid-template-columns:190px 1fr 86px;gap:10px;align-items:center;margin:6px 0}
.hb .lbl{font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.hb .val{text-align:right;font-size:12px;color:#4e5969;font-variant-numeric:tabular-nums}
.cols{display:grid;grid-template-columns:1fr 1fr;gap:16px}
.cols3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:16px}
details{margin:8px 0}
summary{cursor:pointer;color:#2a82da;font-size:12px;outline:none}
.pill{display:inline-block;padding:1px 7px;border-radius:9px;background:#eef4fc;color:#2a82da;
  font-size:11px;margin-left:6px}
.sample{font-size:12.5px;color:#4e5969;padding:4px 0 4px 8px;border-left:2px solid #e8eaed;margin:4px 0}
.note{font-size:12px;color:#8a9099;line-height:1.8}
.scroll{max-height:520px;overflow:auto}
"""


def hbars(rows, unit="", color="", label_w=190, limit=None):
    """rows: [(label, value, text)]"""
    rows = rows[:limit] if limit else rows
    top = max([r[1] for r in rows] or [1]) or 1
    out = []
    for label, value, text in rows:
        pct = value * 100.0 / top
        out.append(
            '<div class="hb"><div class="lbl" title="%s">%s</div>'
            '<div><span class="bar %s" style="width:%.1f%%"></span></div>'
            '<div class="val">%s</div></div>'
            % (esc(label), esc(label), color, max(pct, 0.6), esc(text)))
    return "\n".join(out)


def hour_chart(hours, color="#2a82da"):
    top = max([hours.get(h, 0) for h in range(24)] or [1]) or 1
    cells = []
    for h in range(24):
        v = hours.get(h, 0)
        hgt = max(2, 64 * v / top)
        cells.append(
            '<div style="flex:1;display:flex;flex-direction:column;justify-content:flex-end;'
            'align-items:center;gap:4px" title="%d:00 · %d">'
            '<span style="font-size:10px;color:#8a9099">%s</span>'
            '<span style="width:70%%;height:%.0fpx;background:%s;border-radius:3px;opacity:.85"></span>'
            '<span style="font-size:10px;color:#8a9099">%02d</span></div>'
            % (h, v, v if v else 0, hgt, color, h))
    return ('<div style="display:flex;gap:3px;align-items:flex-end;height:110px;margin-top:6px">'
            + "".join(cells) + "</div>")


def day_chart(days, wecom, chrome):
    """按天：企业微信消息（绿） + Chrome 访问（蓝），双色柱"""
    end = datetime.now().date()
    seq = [end - timedelta(days=i) for i in range(days - 1, -1, -1)]
    wm = wecom["per_day"] if wecom else {}
    cm = chrome["per_day"] if chrome else {}
    # 两条序列量级差 10 倍，各自按自己的峰值归一，否则浏览柱看不出来
    top_w = max([wm.get(d, 0) for d in seq] or [1]) or 1
    top_c = max([cm.get(d, 0) for d in seq] or [1]) or 1
    cells = []
    for d in seq:
        w, c = wm.get(d, 0), cm.get(d, 0)
        cells.append(
            '<div style="flex:1;display:flex;flex-direction:column;justify-content:flex-end;'
            'align-items:center;gap:2px" title="%s · 消息 %d · 浏览 %d">'
            '<div style="display:flex;gap:1px;align-items:flex-end;height:78px">'
            '<span style="width:6px;height:%.0fpx;background:#5ac18e;border-radius:2px"></span>'
            '<span style="width:6px;height:%.0fpx;background:#2a82da;border-radius:2px"></span>'
            '</div><span style="font-size:9px;color:#8a9099">%s</span></div>'
            % (d.strftime("%Y-%m-%d"), w, c,
               max(1.5, 78 * w / top_w), max(1.5, 78 * c / top_c), d.strftime("%m-%d")))
    return ('<div style="display:flex;gap:2px;align-items:flex-end;height:104px">'
            + "".join(cells) + "</div>"
            + '<div class="note" style="margin-top:8px">'
              '<span style="color:#5ac18e">■</span> 企业微信消息（峰值 %d 条）　'
              '<span style="color:#2a82da">■</span> Chrome 访问（峰值 %d 次）　'
              '<span class="sub2">两条序列各自归一，只比趋势不比绝对值</span></div>'
              % (top_w, top_c))


def render(days, wecom, err_w, chrome, err_c, usage):
    since = datetime.now() - timedelta(days=days)
    now = datetime.now()
    parts = []
    A = parts.append

    A("<h1>工作内容分析</h1>")
    A('<div class="sub2">%s ～ %s · 共 %d 天 · 生成于 %s · 企业微信 + Chrome + 任务栏切换，全部本地读取</div>'
      % (since.strftime("%Y-%m-%d"), now.strftime("%Y-%m-%d"), days,
         now.strftime("%Y-%m-%d %H:%M")))

    # KPI
    A('<h2>概览</h2><div class="kpi">')
    if wecom:
        top_conv = wecom["conv_rows"][0] if wecom["conv_rows"] else None
        A('<div><b>%s</b><span>企业微信消息</span></div>' % format(wecom["total"], ","))
        A('<div><b>%d</b><span>活跃会话</span></div>' % wecom["conversations"])
        A('<div><b>%s</b><span>最活跃会话 · %d 条</span></div>'
          % (esc(top_conv["name"][:14]) if top_conv else "—", top_conv["n"] if top_conv else 0))
        A('<div><b>%.0f%%</b><span>我发起的占比</span></div>'
          % (wecom["mine"] * 100.0 / max(1, wecom["total"])))
    else:
        A('<div><b>—</b><span>企业微信不可用</span></div>' * 4)
    A("</div>")
    if err_w:
        A('<div class="note">企业微信：%s</div>' % esc(err_w))

    A('<div class="kpi" style="margin-top:12px">')
    if chrome:
        top_site = chrome["site_visits"].most_common(1)
        A('<div><b>%s</b><span>Chrome 访问次数</span></div>' % format(chrome["visits"], ","))
        A('<div><b>%d</b><span>网站数 · %d 个页面</span></div>'
          % (chrome["sites"], chrome["pages"]))
        A('<div><b>%s</b><span>最常上 · %d 次</span></div>'
          % (esc(top_site[0][0]) if top_site else "—", top_site[0][1] if top_site else 0))
        A('<div><b>%s</b><span>浏览时长（估计）</span></div>'
          % fmt_dur(sum(chrome["site_time"].values())))
    else:
        A('<div><b>—</b><span>Chrome 历史不可用</span></div>' * 4)
    A("</div>")
    if err_c:
        A('<div class="note">Chrome：%s</div>' % esc(err_c))

    if (wecom or chrome) and days <= 60:
        A('<h2>每天的节奏<span class="sub">企业微信消息量 vs Chrome 访问次数</span></h2>')
        A('<div class="card">%s' % day_chart(days, wecom, chrome))
        if chrome and chrome.get("covered_days", days) < days:
            A('<div class="note">Chrome 历史只覆盖 %s ～ %s（%d 天有记录），更早的日期浏览器里已经没有了，'
              '所以蓝柱前几天是空的，不是没上网。</div>'
              % (chrome["first_day"], chrome["last_day"], chrome["covered_days"]))
        A("</div>")

    # 企业微信：跟谁聊
    if wecom:
        A('<h2>企业微信 · 跟谁聊<span class="sub">按消息量排序，含群和单聊</span></h2>')
        A('<div class="card scroll"><table><thead><tr>'
          '<th>会话</th><th class="num">消息</th><th class="num">我发</th><th class="num">对方</th>'
          '<th class="num">活跃天</th><th>主要话题</th><th>最后</th></tr></thead><tbody>')
        for r in wecom["conv_rows"][:40]:
            share = r["mine"] * 100.0 / max(1, r["n"])
            A('<tr><td title="%s">%s<span class="sub2">%s</span></td><td class="num">%d</td>'
              '<td class="num">%d<span class="sub2"> (%.0f%%)</span></td>'
              '<td class="num">%d</td><td class="num">%d</td><td>%s</td><td>%s</td></tr>'
              % (esc(r["name"]), esc(r["name"][:34]), " · 单聊" if r["single"] else "",
                 r["n"], r["mine"], share, r["theirs"], r["active_days"],
                 esc(r["topic"]), esc(r["last"])))
        A("</tbody></table></div>")
        A('<div class="note">共 %d 个会话有消息，上面列前 40 个（占全部消息 %.0f%%）。'
          '「我发」占比低说明多数是群里被动接收；高说明是你在主导推进。</div>'
          % (len(wecom["conv_rows"]),
             sum(r["n"] for r in wecom["conv_rows"][:40]) * 100.0 / max(1, wecom["total"])))

        if wecom.get("people"):
            A('<h2>企业微信 · 单聊对象<span class="sub">一对一会话，按消息量排序；群聊不算在这里</span></h2>')
            A('<div class="card"><table><thead><tr><th>同事</th><th class="num">消息</th>'
              '<th class="num">我发</th><th class="num">对方</th><th class="num">活跃天</th>'
              '<th>主要话题</th><th>最后</th></tr></thead><tbody>')
            for r in wecom["people"][:20]:
                A('<tr><td>%s</td><td class="num">%d</td><td class="num">%d</td>'
                  '<td class="num">%d</td><td class="num">%d</td><td>%s</td><td>%s</td></tr>'
                  % (esc(r["name"]), r["n"], r["mine"], r["theirs"], r["active_days"],
                     esc(r["topic"]), esc(r["last"])))
            A('</tbody></table></div>')
            total_single = sum(r["n"] for r in wecom["people"])
            A('<div class="note">单聊共 %d 人、%d 条，占全部消息 %.0f%%；'
              '单人消息量高说明是直接对接的同事/客户，群里的人不算在这里。</div>'
              % (len(wecom["people"]), total_single,
                 total_single * 100.0 / max(1, wecom["total"])))

        # 工作内容
        A('<h2>企业微信 · 聊什么<span class="sub">按关键词给每条消息归类，一条只归一类</span></h2>')
        A('<div class="card">')
        rows = [(t["topic"], t["n"], "%d 条 · %.0f%%" % (t["n"], t["n"] * 100.0 / max(1, wecom["total"])))
                for t in wecom["topic_rows"]]
        A(hbars(rows, color="p"))
        A("</div>")
        A('<div class="cols">')
        for t in wecom["topic_rows"]:
            if t["topic"] == "其他" or not t["n"]:
                continue
            A('<div class="card"><b>%s</b><span class="pill">%d 条</span>' % (esc(t["topic"]), t["n"]))
            if t["convs"]:
                A('<div class="note">主要在：%s</div>'
                  % "、".join("%s(%d)" % (esc(c[:18]), n) for c, n in t["convs"]))
            for when, conv, sender, text in t["samples"]:
                A('<div class="sample">[%s] %s · %s<br>%s</div>'
                  % (esc(when), esc(conv[:14]), esc(sender[:8]), esc(text[:150])))
            A("</div>")
        A("</div>")

        A('<h2>企业微信 · 什么时候在聊<span class="sub">按消息发送时间</span></h2><div class="card">')
        A(hour_chart(wecom["hours"], "#5ac18e"))
        busy = max(range(24), key=lambda h: wecom["hours"].get(h, 0))
        night = sum(wecom["hours"].get(h, 0) for h in (20, 21, 22, 23, 0, 1, 2, 3))
        A('<div class="note">消息最集中的时段：%02d:00 前后（%d 条）；'
          '9–19 点占 %.0f%%，20 点后到次日 4 点共 %d 条（%.1f%%）。</div>'
          % (busy, wecom["hours"].get(busy, 0),
             sum(wecom["hours"].get(h, 0) for h in range(9, 20)) * 100.0 / max(1, wecom["total"]),
             night, night * 100.0 / max(1, wecom["total"])))
        A("</div>")

    # Chrome
    if chrome:
        A('<h2>Chrome · 上哪些网站<span class="sub">按访问次数；时长是按相邻访问间隔估算的上限</span></h2>')
        A('<div class="card">')
        rows = [(site, n, "%d 次 · %s" % (n, fmt_dur(chrome["site_time"][site])))
                for site, n in chrome["site_visits"].most_common(18)]
        A(hbars(rows, color=""))
        kinds = Counter()
        for site, n in chrome["site_visits"].items():
            kinds[site_kind(site)] += n
        A('<div class="note">按用途分：%s。工作系统指 Gerrit/工单/协作/代码托管这类，'
          '查资料指搜索结果和文档站。访问次数不等于时长，看结构即可。</div>'
          % "、".join("%s %d 次（%.0f%%）" % (k, v, v * 100.0 / max(1, chrome["visits"]))
                     for k, v in kinds.most_common()))
        A("</div>")

        A('<h2>Chrome · 具体页面<span class="sub">标题来自浏览器历史，同一页面只看一次</span></h2>')
        A('<div class="card scroll"><table><thead><tr>'
          '<th>页面</th><th class="num">次数</th><th class="num">估计停留</th><th>站点</th>'
          '</tr></thead><tbody>')
        for url, n in chrome["page_visits"].most_common(35):
            title = chrome["page_title"].get(url) or url
            host = urlparse(url).hostname or ""
            A('<tr><td title="%s">%s</td><td class="num">%d</td><td class="num">%s</td>'
              '<td>%s</td></tr>'
              % (esc(url), esc(title[:70]), n, fmt_dur(chrome["page_time"][url]),
                 esc(site_of(host))))
        A("</tbody></table></div>")

        A('<h2>Chrome · 什么时候在上网<span class="sub">按访问时间</span></h2><div class="card">')
        A(hour_chart(chrome["hours"], "#2a82da"))
        busy = max(range(24), key=lambda h: chrome["hours"].get(h, 0))
        out = chrome["visits"] - sum(chrome["hours"].get(h, 0) for h in range(9, 20))
        A('<div class="note">访问最集中的时段：%02d:00 前后；9–19 点占 %.0f%%，'
          '其余时段 %d 次。</div>'
          % (busy, sum(chrome["hours"].get(h, 0) for h in range(9, 20)) * 100.0
             / max(1, chrome["visits"]), out))
        A("</div>")

    # 交叉：任务栏侧数据
    if usage and (usage["chat"] or usage["apps"]):
        A('<h2>和任务栏统计对照<span class="sub">企业微信内部会话切换次数 vs 消息量；应用在岗时长</span></h2>')
        A('<div class="cols">')
        A('<div class="card"><b>企业微信内切换最多的会话</b>')
        msg_by_conv = {r["name"]: r["n"] for r in (wecom["conv_rows"] if wecom else [])}
        rows = []
        for name, n in usage["chat"][:12]:
            msg = msg_by_conv.get(name)
            rows.append((name, n, "%d 次" % n + (" · 消息 %d" % msg if msg else "")))
        A(hbars(rows, color="o") if rows else '<div class="note">暂无</div>')
        A('<div class="note">切换次数高但消息少的会话，通常是频繁瞄一眼的群；'
          '消息多但切换少的是长时间停留的主战场。</div></div>')
        A('<div class="card"><b>应用使用时长</b>')
        rows = [(app, ms / 60000.0, fmt_dur(ms / 1000.0)) for app, ms in usage["apps"]]
        A(hbars(rows, color="g") if rows else '<div class="note">暂无</div>')
        A("</div></div>")

    # 口径
    A('<h2>统计口径</h2><div class="card note">')
    A("· 企业微信：读取本机 message.db（wxSQLite3 加密，用进程内存里的 key 解密，WAL 快照保证一致），"
      "只统计最近 %d 天、类型为文本/系统消息的记录，不读图片和文件内容。<br>" % days)
    A("· 话题分类是关键词规则，一条消息只归一类，都会有不少落进「其他」"
      "（闲聊、表情、简短回复），占比只用于看相对结构。<br>")
    A("· 消息正文命中不了关键词时，按所在会话名归类（工单群、项目群、评审群等），"
      "所以「工单/客户」包含整段群聊，不全是工单本身。<br>")
    A("· 8 个字以内的短消息单独归到「短回复/闲聊」，避免把「好的」「收到」算进工作内容。<br>")
    A("· Chrome：复制 History 库后读取 visits 表；停留时长用「到下一次访问的间隔」估算，"
      "同站点封顶 5 分钟、跨站点封顶 2 分钟，只反映量级，不等于真实阅读时长。<br>")
    A("· 页面标题是浏览器记录的原始 title，未做清洗；内网地址不在此页面展开，只写用途名。")
    A("</div>")

    return ("<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
            "<title>工作内容分析</title><style>%s</style></head><body><div class=\"wrap\">%s"
            "</div></body></html>" % (CSS, "\n".join(parts)))


def main():
    ap = argparse.ArgumentParser(description="工作内容分析报告")
    ap.add_argument("--days", type=int, default=30)
    ap.add_argument("--out", default=os.path.expanduser("~/下载/工作内容分析.html"))
    ap.add_argument("--json", help="把摘要数据另存为 JSON")
    args = ap.parse_args()

    since = (datetime.now() - timedelta(days=args.days)).timestamp()
    wecom, err_w = load_wecom(args.days)
    chrome, err_c = load_chrome(since)
    usage = load_usage(args.days)
    doc = render(args.days, wecom, err_w, chrome, err_c, usage)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(doc)
    print("报告：%s（%.0f KB）" % (args.out, len(doc.encode()) / 1024))
    if wecom:
        print("企业微信 %d 条消息 / %d 会话" % (wecom["total"], wecom["conversations"]))
    if chrome:
        print("Chrome %d 次访问 / %d 站点 / %d 页面"
              % (chrome["visits"], chrome["sites"], chrome["pages"]))
    if args.json:
        data = {}
        if wecom:
            data["wecom"] = {"total": wecom["total"], "mine": wecom["mine"],
                             "conversations": wecom["conv_rows"][:50],
                             "topics": [{"topic": t["topic"], "n": t["n"]}
                                        for t in wecom["topic_rows"]],
                             "hours": dict(wecom["hours"])}
        if chrome:
            data["chrome"] = {"visits": chrome["visits"],
                              "sites": chrome["site_visits"].most_common(40),
                              "pages": [{"url": u, "title": chrome["page_title"].get(u),
                                         "n": n} for u, n in chrome["page_visits"].most_common(40)],
                              "hours": dict(chrome["hours"])}
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(data, fh, ensure_ascii=False, indent=2)
        print("摘要：%s" % args.json)


if __name__ == "__main__":
    main()
