# 应用使用时间（app-usage-monitor）

> 统信 UOS / Deepin 深度任务栏（dde-dock）插件：自动记录每个应用的使用时长，并在托盘弹窗里用图表展示「一天 24 小时到底是怎么过的」。

![界面截图](docs/screenshot.png)

## 特性

- **自动统计，无需手动操作**：任务栏后台每 3 秒采样一次当前焦点窗口，按应用累计使用时长，每 10 秒落盘一次。
- **操作 / 查看 两种口径**：有键鼠输入算「操作」，窗口开着但没输入算「查看」，可据此判断「人在不在电脑前」。
- **锁屏时间单独记录**：通过 `com.deepin.SessionManager` 的 `Locked` 属性采集，锁屏期间不计入任何应用。
- **每小时用时图**：横轴 00–23 时，纵轴 0–60 分，蓝=操作、橙=查看、灰=锁屏堆叠显示。**没走完的小时只画实际分钟数，不会被归一化成 100%**。
- **三种视图**：`时长`（三段堆叠）/ `在岗`（只看操作+查看）/ `曲线`（三条折线对照）；鼠标悬停任意小时可看到精确分钟数与占比。
- **主导状态色带**：图下方 24 格色带，按每小时占主导的状态着色，一眼看出连续的工作时段与离开时段。
- **日 / 月两种视图**：日视图看当日明细与每小时构成，月视图看整月每天的总量。
- **中文应用名**：解析 `.desktop` 文件拿本地化名称与图标，而不是显示 `chrome`、`code` 这类进程名。
- **纯本地**：所有数据只写本机 SQLite，不联网、不上传。

## 界面说明

弹窗从上到下：

| 区域 | 内容 |
| --- | --- |
| 标题栏 | 日期 + 星期、`月视图` 切换 |
| 统计摘要 | 共使用 / 操作 / 查看 / 锁屏时间 |
| 每小时用时（分） | 24 根堆叠柱 + 主导状态色带，可切换 `时长` / `在岗` / `曲线` |
| 应用列表 | 按总时长排序，含时长条与「操作 / 查看」细分，滚轮可滚动 |

## 采集口径

采样周期 3 秒，每次读取焦点窗口与系统空闲时长，把两个采样点之间的时间记到「上一次"的窗口」上：

| 场景 | 判定 | 计入 |
| --- | --- | --- |
| 会话已锁屏 | `Locked == true` | 锁屏时间（不记给任何应用） |
| 空闲时长增量 ≥ 采样间隔的 3/5 | 无输入，只是开着窗口 | 查看 |
| 其余情况 | 有键鼠输入 | 操作 |
| 无焦点窗口（桌面） | `_NET_ACTIVE_WINDOW` 为空 | 不计时 |

> ⚠️ 「查看」的判定是「窗口有焦点但没有输入」，所以**开着窗口去吃饭**也会被算成查看时间，而不是离开。这是刻意的取舍：想统计「离开」请直接锁屏。

单次采样的时间增量上限为 4 个采样周期（12 秒），避免休眠唤醒后把大段时间错误地记到某个应用上。

## 数据存储

数据库：`~/.local/share/app-usage-monitor/usage.db`（SQLite，首次运行时自动创建）

```sql
-- 应用 × 天
CREATE TABLE usage (
    date TEXT NOT NULL,
    app TEXT NOT NULL,            -- 稳定标识（WM_CLASS / 进程名，小写）
    display_name TEXT,            -- 本地化展示名
    icon TEXT,
    active_ms INTEGER,            -- 操作毫秒
    viewing_ms INTEGER,           -- 查看毫秒
    PRIMARY KEY (date, app)
);

-- 天 × 锁屏
CREATE TABLE locks (
    date TEXT PRIMARY KEY,
    lock_ms INTEGER
);

-- 天 × 小时，图表数据源
CREATE TABLE hourly_usage (
    date TEXT NOT NULL,
    hour INTEGER NOT NULL,        -- 0-23
    active_ms INTEGER,
    viewing_ms INTEGER,
    lock_ms INTEGER,
    PRIMARY KEY (date, hour)
);
```

## 依赖

- Qt 5.11+（Widgets、Sql、DBus）
- `dde-dock` 开发包（`dde-dock.pc`）
- X11 开发包（`x11.pc`）
- 运行时：`xprintidle`（用于读取空闲时长）

Debian / UOS 系安装：

```bash
sudo apt install build-essential cmake qtbase5-dev libqt5sql5-sqlite \
                 libdde-dock-dev libx11-dev x11-utils
```

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
# 产物：build/out/libapp-usage-monitor.so
```

## 安装

```bash
sudo ./install.sh
```

脚本会完成构建 → 把 `.so` 装到 `/usr/lib/dde-dock/plugins/` → 重启 `dde-dock`。

手动安装：

```bash
sudo install -m 755 build/out/libapp-usage-monitor.so /usr/lib/dde-dock/plugins/
kill "$(pgrep -x dde-dock)"   # 由 startdde 自动拉起
```

装好后托盘会出现一个三根柱子的图标，点击即弹窗。

## 卸载

```bash
sudo rm /usr/lib/dde-dock/plugins/libapp-usage-monitor.so
kill "$(pgrep -x dde-dock)"
rm -rf ~/.local/share/app-usage-monitor   # 连同历史数据一起删除（可选）
```

## 目录结构

```
.
├── src/
│   ├── appusagemonitorplugin.*   # dde-dock 插件入口、托盘图标、弹窗装配
│   ├── usagetracker.*            # 3 秒采样、操作/查看/锁屏判定、攒批落盘
│   ├── x11windowinfo.*           # X11 读取焦点窗口与所属进程
│   ├── idledetector.*            # 调用 xprintidle 读空闲时长
│   ├── appregistry.*             # .desktop 解析：本地化名称与图标
│   ├── usagestorage.*            # SQLite 读写
│   ├── usagepopupwidget.*        # 弹窗 UI + 自绘小时图表控件
│   ├── usagechart.*              # 小时分钟数 / 占比 / 时段文字等纯计算
│   └── usageentry.h              # 数据结构
├── docs/screenshot.png
├── CMakeLists.txt
└── install.sh
```

## 已知限制

- 只支持 X11 会话；Wayland 下 `_NET_ACTIVE_WINDOW` 取不到，需要另走合成器协议。
- 依赖 `xprintidle`，没装时空闲时长恒为 0，所有时间都会被记成「操作」。
- 应用名与图标来自首次识别时的缓存；换语言或换主题后旧记录不会自动刷新。
- 修改空闲判定阈值只影响之后的数据，历史记录不会重算。

## 许可证

[MIT](LICENSE)
