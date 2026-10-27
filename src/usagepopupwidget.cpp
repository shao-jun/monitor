#include "usagepopupwidget.h"

#include "usagechart.h"
#include "usagestorage.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStackedWidget>
#ifndef APP_USAGE_VERSION
#define APP_USAGE_VERSION "1.1.1"   // 由 CMake 定义，单独编译源码时的兜底
#endif

#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int kRowHeight = 48;
}

static QString formatShort(qint64 ms)
{
    const qint64 s = ms / 1000;
    const qint64 h = s / 3600;
    const qint64 m = (s % 3600) / 60;
    if (h > 0)
        return m > 0 ? QStringLiteral("%1小时%2分").arg(h).arg(m) : QStringLiteral("%1小时").arg(h);
    if (m > 0)
        return QStringLiteral("%1分").arg(m);
    if (s > 0)
        return QStringLiteral("%1秒").arg(s);
    return QStringLiteral("0分");
}

static QIcon appIcon(const QString &icon)
{
    if (icon.isEmpty())
        return QIcon::fromTheme(QStringLiteral("application-x-executable"));
    if (icon.startsWith('/'))
        return QIcon(icon);
    const QIcon themed = QIcon::fromTheme(icon);
    return themed.isNull() ? QIcon::fromTheme(QStringLiteral("application-x-executable")) : themed;
}

static void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            // deleteLater 要到下一轮事件循环才真正销毁，先隐藏并及时不再参与绘制，
            // 否则旧内容会和新内容叠在一起闪一下（切月份时尤其明显）
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        } else if (QLayout *l = item->layout()) {
            clearLayout(l);
            delete l;
        }
        delete item;
    }
}

static QColor primaryColor() { return QColor(Qt::black); }
static QColor secondaryColor() { return QColor(Qt::black); }
static QString primaryText() { return primaryColor().name(); }
static QString secondaryText() { return secondaryColor().name(); }

class UsageBarWidget : public QWidget
{
public:
    explicit UsageBarWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedHeight(2);
    }

    void setValues(qint64 active, qint64 viewing)
    {
        m_active = active;
        m_viewing = viewing;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect r = rect();
        const qint64 total = m_active + m_viewing;
        if (total <= 0) {
            p.fillRect(r, QColor(0, 0, 0, 24));
            return;
        }
        const int activeW = qRound(r.width() * (double)m_active / (double)total);
        p.fillRect(QRect(r.left(), r.top(), activeW, r.height()), QColor(0x2a, 0x82, 0xda));
        p.fillRect(QRect(r.left() + activeW, r.top(), r.width() - activeW, r.height()), QColor(0x9c, 0xc8, 0xf0));
    }

private:
    qint64 m_active = 0;
    qint64 m_viewing = 0;
};

class AppRowWidget : public QWidget
{
public:
    explicit AppRowWidget(const AppUsageRow &row, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedHeight(kRowHeight);

        QHBoxLayout *h = new QHBoxLayout(this);
        h->setContentsMargins(0, 4, 0, 4);
        h->setSpacing(8);

        QLabel *icon = new QLabel;
        icon->setPixmap(appIcon(row.icon).pixmap(24, 24));
        icon->setFixedSize(24, 24);
        h->addWidget(icon, 0, Qt::AlignTop);

        QVBoxLayout *v = new QVBoxLayout;
        v->setSpacing(2);

        QHBoxLayout *top = new QHBoxLayout;
        QLabel *name = new QLabel(row.displayName);
        name->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(primaryText()));
        QLabel *time = new QLabel(formatShort(row.totalMs()));
        time->setStyleSheet(QStringLiteral("color:%1;").arg(secondaryText()));
        top->addWidget(name);
        top->addStretch();
        top->addWidget(time);
        v->addLayout(top);

        UsageBarWidget *bar = new UsageBarWidget;
        bar->setValues(row.activeMs, row.viewingMs);
        v->addWidget(bar);

        QLabel *detail = new QLabel(QStringLiteral("操作 %1 · 查看 %2")
                                        .arg(formatShort(row.activeMs))
                                        .arg(formatShort(row.viewingMs)));
        detail->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(secondaryText()));
        v->addWidget(detail);

        h->addLayout(v, 1);
    }
};

class MonthlyChartWidget : public QWidget
{
public:
    explicit MonthlyChartWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedHeight(132);
        setMouseTracking(true);
    }

    // volumes: 每天的总用时（毫秒），day 从 1 开始；daysInMonth 为当月天数
    void setData(const QVector<QPair<int, qint64>> &volumes, int daysInMonth, int todayDay = -1)
    {
        m_data = volumes;
        m_days = daysInMonth;
        m_today = todayDay;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);

        const QRectF full = rect();
        QFont axisFont = font();
        axisFont.setPixelSize(8);
        p.setFont(axisFont);

        if (m_data.isEmpty() || m_days <= 0) {
            p.setPen(secondaryColor());
            p.drawText(full, Qt::AlignCenter, QStringLiteral("本月暂无记录"));
            return;
        }

        qint64 maxTotal = 0;
        QMap<int, qint64> byDay;
        for (const auto &e : m_data) {
            byDay.insert(e.first, e.second);
            maxTotal = qMax(maxTotal, e.second);
        }
        if (maxTotal <= 0)
            maxTotal = 1;

        // 留出顶部数值、底部日期刻度的空间
        const QRectF plot(full.left() + 4.0, full.top() + 11.0,
                          full.width() - 8.0, full.height() - 11.0 - 13.0);
        if (plot.height() <= 4.0)
            return;

        // 顶部：量级参考
        p.setPen(secondaryColor());
        p.drawText(QRectF(full.left(), full.top(), full.width(), 10.0),
                   Qt::AlignLeft | Qt::AlignVCenter, formatShort(maxTotal));

        // 基线
        p.setPen(QPen(QColor(0, 0, 0, 36), 1.0));
        p.drawLine(QPointF(plot.left(), plot.bottom()), QPointF(plot.right(), plot.bottom()));

        const double slotW = plot.width() / m_days;
        const double barW = qMax(2.0, slotW * 0.68);
        for (int day = 1; day <= m_days; ++day) {
            const qint64 total = byDay.value(day, 0);
            if (total <= 0)
                continue;
            const double h = qMax(1.0, double(total) / double(maxTotal) * plot.height());
            const double x = plot.left() + (day - 1) * slotW + (slotW - barW) / 2.0;
            const bool isToday = (day == m_today);
            p.fillRect(QRectF(x, plot.bottom() - h, barW, h),
                       isToday ? QColor(0x2a, 0x82, 0xda) : QColor(0x2a, 0x82, 0xda, 165));
        }

        // 底部日期刻度：1 / 5 / 10 / 15 / 20 / 25 / 月末
        p.setPen(secondaryColor());
        QVector<int> ticks{1, 5, 10, 15, 20, 25};
        if (m_days > 25)
            ticks.append(m_days);
        for (int day : ticks) {
            if (day < 1 || day > m_days)
                continue;
            const double x = plot.left() + (day - 0.5) * slotW;
            p.drawText(QRectF(x - 8.0, plot.bottom() + 2.0, 16.0, 10.0), Qt::AlignCenter,
                       QString::number(day));
        }
    }

private:
    QVector<QPair<int, qint64>> m_data;
    QMap<int, qint64> m_byDay;
    int m_days = 0;
    int m_today = -1;
};

// 一天 24 小时的用时图：纵轴是分钟（满格 60 分），默认堆叠柱（蓝=操作 橙=查看 灰=锁屏）。
// 没走完/没记录满的小时只画出实际用掉的分钟数，不会被归一化成 100%。
// 可切「在岗」（去掉锁屏，只看操作/查看分钟）与「曲线」；下方附按主导状态着色的状态条。
class HourlyChartWidget : public QWidget
{
public:
    enum class Mode { Stacked, Awake, Line };

    explicit HourlyChartWidget(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_data(24)
    {
        setFixedHeight(168);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setToolTip(QStringLiteral("每小时用时（分钟）：操作 + 查看 + 锁屏，满格 60 分"));
    }

    void setRows(const QVector<HourlyUsageRow> &rows)
    {
        m_data = hourlyPercents(rows);
        m_hasData = false;
        for (const HourlyPercent &p : m_data) {
            if (p.totalMin >= 0.0) {
                m_hasData = true;
                break;
            }
        }
        m_hoverHour = -1;
        update();
    }

    void setMode(Mode mode)
    {
        m_mode = mode;
        update();
    }

protected:
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int hour = hourAt(e->pos());
        if (hour != m_hoverHour) {
            m_hoverHour = hour;
            if (hour >= 0 && hour < 24 && m_data.at(hour).totalMin >= 0.0)
                setToolTip(hourDetail(m_data.at(hour), hour));
            else
                setToolTip(QStringLiteral("每小时用时（分钟）：操作 + 查看 + 锁屏，满格 60 分"));
            update();
        }
        QWidget::mouseMoveEvent(e);
    }

    void leaveEvent(QEvent *e) override
    {
        m_hoverHour = -1;
        update();
        QWidget::leaveEvent(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);

        const QRectF full = rect();
        const QColor activeColor(0x2a, 0x82, 0xda);
        const QColor viewingColor(0xf0, 0xa2, 0x4a);
        const QColor lockColor(0xc3, 0xc8, 0xce);
        const bool withLock = (m_mode != Mode::Awake);

        // 图例
        QFont legendFont = font();
        legendFont.setPixelSize(10);
        p.setFont(legendFont);
        const struct {
            QColor color;
            QString label;
        } legend[3] = {{activeColor, QStringLiteral("操作")},
                       {viewingColor, QStringLiteral("查看")},
                       {lockColor, QStringLiteral("锁屏")}};
        qreal lx = full.left() + 1.0;
        const qreal legendY = full.top() + 6.0;
        for (int i = 0; i < (withLock ? 3 : 2); ++i) {
            p.setPen(Qt::NoPen);
            p.setBrush(legend[i].color);
            p.drawRoundedRect(QRectF(lx, legendY - 4.0, 9.0, 9.0), 2.0, 2.0);
            p.setPen(secondaryColor());
            p.drawText(QPointF(lx + 13.0, legendY + 4.0), legend[i].label);
            lx += 20.0 + QFontMetricsF(legendFont).width(legend[i].label) + 10.0;
        }

        const qreal left = full.left() + 26.0;
        const qreal right = full.right() - 2.0;
        const qreal top = full.top() + 18.0;
        const qreal plotBottom = full.bottom() - 40.0;
        const qreal ribbonTop = plotBottom + 4.0;
        const qreal ribbonH = 8.0;
        const qreal axisLabelY = ribbonTop + ribbonH + 12.0;
        if (right <= left || plotBottom <= top)
            return;

        const qreal slot = (right - left) / 24.0;
        m_slot = slot;
        m_left = left;
        const auto xOf = [&](int hour) { return left + (hour + 0.5) * slot; };
        const auto yOf = [&](double minutes) {
            return plotBottom - (qMin(minutes, 60.0) / 60.0) * (plotBottom - top);
        };

        // 悬停高亮
        if (m_hoverHour >= 0) {
            p.fillRect(QRectF(left + m_hoverHour * slot, top, slot, plotBottom - top),
                       QColor(0x2a, 0x82, 0xda, 16));
        }

        // 横向网格与纵轴刻度
        QFont axisFont = font();
        axisFont.setPixelSize(8);
        p.setFont(axisFont);
        for (int value = 0; value <= 60; value += 15) {
            const qreal y = yOf(value);
            QPen grid(QColor(0, 0, 0, 24));
            grid.setWidthF(1.0);
            grid.setStyle(value == 0 ? Qt::SolidLine : Qt::DashLine);
            p.setPen(grid);
            p.drawLine(QPointF(left, y), QPointF(right, y));
            p.setPen(secondaryColor());
            p.drawText(QRectF(full.left(), y - 6.0, left - full.left() - 3.0, 12.0),
                       Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("%1").arg(value));
        }

        if (!m_hasData) {
            p.setPen(secondaryColor());
            p.drawText(QRectF(left, top, right - left, plotBottom - top), Qt::AlignCenter,
                       QStringLiteral("当天暂无小时记录"));
            return;
        }

        if (m_mode != Mode::Line) {
            const qreal bar = qMax(3.0, slot * 0.62);
            for (int hour = 0; hour < 24; ++hour) {
                const HourlyPercent &hp = m_data.at(hour);
                const qreal x = xOf(hour) - bar / 2.0;
                if (hp.totalMin < 0.0) {
                    p.fillRect(QRectF(x, plotBottom - 2.0, bar, 2.0), QColor(0, 0, 0, 30));
                    continue;
                }
                double base = 0.0;
                double segs[3] = {hp.activeMin, hp.viewingMin, hp.lockMin};
                QColor colors[3] = {activeColor, viewingColor, lockColor};
                int count = 3;
                if (m_mode == Mode::Awake) {
                    segs[0] = hp.activeMin;
                    segs[1] = hp.viewingMin;
                    count = 2;
                }
                for (int s = 0; s < count; ++s) {
                    if (segs[s] <= 0.01)
                        continue;
                    const qreal yTopSeg = yOf(base + segs[s]);
                    const qreal h = yOf(base) - yTopSeg;
                    p.fillRect(QRectF(x, yTopSeg, bar, h), colors[s]);
                    base += segs[s];
                }
            }
        } else {
            const auto drawSeries = [&](double HourlyPercent::*field, const QColor &color) {
                QPen pen(color);
                pen.setWidthF(1.6);
                pen.setCapStyle(Qt::RoundCap);
                pen.setJoinStyle(Qt::RoundJoin);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);

                QPainterPath path;
                bool started = false;
                QVector<QPointF> points;
                for (int hour = 0; hour < 24; ++hour) {
                    if (m_data.at(hour).totalMin < 0.0) {
                        started = false;
                        continue;
                    }
                    const double value = m_data.at(hour).*field;
                    if (value < 0.0) {
                        started = false;
                        continue;
                    }
                    const QPointF pt(xOf(hour), yOf(value));
                    if (!started) {
                        path.moveTo(pt);
                        started = true;
                    } else {
                        path.lineTo(pt);
                    }
                    points.append(pt);
                }
                p.drawPath(path);

                p.setPen(Qt::NoPen);
                p.setBrush(color);
                for (const QPointF &pt : points)
                    p.drawEllipse(pt, 1.5, 1.5);
            };
            drawSeries(&HourlyPercent::lockMin, lockColor);
            drawSeries(&HourlyPercent::viewingMin, viewingColor);
            drawSeries(&HourlyPercent::activeMin, activeColor);
        }

        // 状态条：按每小时的主导状态着色
        for (int hour = 0; hour < 24; ++hour) {
            p.fillRect(QRectF(left + hour * slot, ribbonTop, slot, ribbonH),
                       m_hasData ? stateColorOf(m_data.at(hour)) : QColor(0xee, 0xf0, 0xf3));
        }

        // 横轴时刻（每 3 小时）
        for (int hour = 0; hour < 24; hour += 3) {
            const qreal x = xOf(hour);
            p.setPen(QPen(QColor(0, 0, 0, 40), 1.0));
            p.drawLine(QPointF(x, ribbonTop + ribbonH), QPointF(x, ribbonTop + ribbonH + 3.0));
            p.setPen(secondaryColor());
            p.drawText(QRectF(x - 12.0, axisLabelY - 8.0, 24.0, 11.0), Qt::AlignCenter,
                       QStringLiteral("%1").arg(hour, 2, 10, QLatin1Char('0')));
        }
    }

private:
    static QColor stateColorOf(const HourlyPercent &hp)
    {
        if (hp.totalMin < 0.0)
            return QColor(0xee, 0xf0, 0xf3);
        const int state = dominantState(hp);
        if (state == 0)
            return QColor(0x2a, 0x82, 0xda);
        if (state == 1)
            return QColor(0xf0, 0xa2, 0x4a);
        return QColor(0xc3, 0xc8, 0xce);
    }

    int hourAt(const QPoint &pos) const
    {
        if (m_slot <= 0.0)
            return -1;
        const qreal x = pos.x() - m_left;
        if (x < 0.0)
            return -1;
        const int hour = int(x / m_slot);
        return (hour >= 0 && hour < 24) ? hour : -1;
    }

    QVector<HourlyPercent> m_data;
    bool m_hasData = false;
    Mode m_mode = Mode::Stacked;
    int m_hoverHour = -1;
    qreal m_slot = 0.0;
    qreal m_left = 0.0;
};

UsagePopupWidget::UsagePopupWidget(UsageStorage *storage, QWidget *parent)
    : QWidget(parent)
    , m_storage(storage)
    , m_date(QDate::currentDate())
{
    setFixedWidth(360);
    setMinimumHeight(660);
    setMaximumHeight(760);
    setAttribute(Qt::WA_DeleteOnClose, false);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    QHBoxLayout *header = new QHBoxLayout;
    m_title = new QLabel;
    m_title->setStyleSheet(QStringLiteral("color:%1; font-size:15px; font-weight:600;").arg(primaryText()));
    m_modeBtn = new QPushButton(QStringLiteral("月视图"));
    m_modeBtn->setFlat(true);
    m_modeBtn->setCursor(Qt::PointingHandCursor);
    m_modeBtn->setStyleSheet(QStringLiteral("color:%1;").arg(primaryText()));
    connect(m_modeBtn, &QPushButton::clicked, this, &UsagePopupWidget::toggleMode);
    header->addWidget(m_title);
    header->addStretch();
    header->addWidget(m_modeBtn);

    // 关于：一个小到几乎不占地方的 ⓘ，点开才显示作者和仓库
    m_aboutBtn = new QPushButton(QStringLiteral("\u24d8"));
    m_aboutBtn->setFixedSize(14, 14);
    m_aboutBtn->setFlat(true);
    m_aboutBtn->setCursor(Qt::PointingHandCursor);
    m_aboutBtn->setToolTip(QStringLiteral("关于"));
    m_aboutBtn->setFocusPolicy(Qt::NoFocus);
    m_aboutBtn->setStyleSheet(QStringLiteral("QPushButton{border:none;padding:0;margin:0;color:%1;font-size:12px;}")
                                  .arg(secondaryText()));
    connect(m_aboutBtn, &QPushButton::clicked, this, &UsagePopupWidget::showAbout);
    header->addWidget(m_aboutBtn, 0, Qt::AlignVCenter);
    root->addLayout(header);

    QHBoxLayout *nav = new QHBoxLayout;
    m_prevBtn = new QPushButton(QStringLiteral("◀"));
    m_prevBtn->setFixedSize(28, 28);
    m_prevBtn->setFlat(true);
    m_prevBtn->setStyleSheet(QStringLiteral("color:%1;").arg(primaryText()));
    m_nextBtn = new QPushButton(QStringLiteral("▶"));
    m_nextBtn->setFixedSize(28, 28);
    m_nextBtn->setFlat(true);
    m_nextBtn->setStyleSheet(QStringLiteral("color:%1;").arg(primaryText()));
    m_todayBtn = new QPushButton(QStringLiteral("今天"));
    m_todayBtn->setFlat(true);
    m_todayBtn->setCursor(Qt::PointingHandCursor);
    m_todayBtn->setStyleSheet(QStringLiteral("color:%1;").arg(primaryText()));
    connect(m_prevBtn, &QPushButton::clicked, this, &UsagePopupWidget::prev);
    connect(m_nextBtn, &QPushButton::clicked, this, &UsagePopupWidget::next);
    connect(m_todayBtn, &QPushButton::clicked, this, &UsagePopupWidget::goToday);
    nav->addWidget(m_prevBtn);
    nav->addStretch();
    nav->addWidget(m_todayBtn);
    nav->addStretch();
    nav->addWidget(m_nextBtn);
    root->addLayout(nav);

    m_summary = new QLabel;
    m_summary->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(secondaryText()));
    root->addWidget(m_summary);

    m_lockLabel = new QLabel;
    m_lockLabel->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(secondaryText()));
    root->addWidget(m_lockLabel);

    m_stack = new QStackedWidget;
    root->addWidget(m_stack, 1);

    QWidget *dayPage = new QWidget;
    QVBoxLayout *dayPageLayout = new QVBoxLayout(dayPage);
    dayPageLayout->setContentsMargins(0, 0, 0, 0);
    dayPageLayout->setSpacing(6);

    QHBoxLayout *chartHead = new QHBoxLayout;
    chartHead->setSpacing(4);
    QLabel *chartCaption = new QLabel(QStringLiteral("每小时用时（分）"));
    chartCaption->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(secondaryText()));
    m_stackedBtn = new QPushButton(QStringLiteral("时长"));
    m_awakeBtn = new QPushButton(QStringLiteral("在岗"));
    m_lineBtn = new QPushButton(QStringLiteral("曲线"));
    for (QPushButton *b : {m_stackedBtn, m_awakeBtn, m_lineBtn}) {
        b->setCheckable(true);
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral(
            "QPushButton{color:%1; font-size:11px; border:1px solid rgba(0,0,0,0.14);"
            " border-radius:9px; padding:1px 9px;}"
            "QPushButton:checked{background:#2a82da; border-color:#2a82da; color:#ffffff;}")
            .arg(primaryText()));
    }
    m_stackedBtn->setChecked(true);
    connect(m_stackedBtn, &QPushButton::clicked, this, [this]() { setChartMode(0); });
    connect(m_awakeBtn, &QPushButton::clicked, this, [this]() { setChartMode(1); });
    connect(m_lineBtn, &QPushButton::clicked, this, [this]() { setChartMode(2); });
    chartHead->addWidget(chartCaption);
    chartHead->addStretch();
    chartHead->addWidget(m_stackedBtn);
    chartHead->addWidget(m_awakeBtn);
    chartHead->addWidget(m_lineBtn);
    dayPageLayout->addLayout(chartHead);

    m_hourChart = new HourlyChartWidget;
    dayPageLayout->addWidget(m_hourChart);

    QWidget *dayList = new QWidget;
    m_dayLayout = new QVBoxLayout(dayList);
    m_dayLayout->setContentsMargins(0, 0, 0, 0);
    m_dayLayout->setSpacing(0);
    QScrollArea *dayScroll = new QScrollArea;
    dayScroll->setWidgetResizable(true);
    dayScroll->setFrameShape(QFrame::NoFrame);
    dayScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 滚动条隐藏，仍可用滚轮/触摸滚动
    dayScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    dayScroll->setStyleSheet(QStringLiteral("QScrollArea{background:transparent;} QScrollArea>QWidget>QWidget{background:transparent;}"));
    dayScroll->viewport()->setAutoFillBackground(false);
    dayScroll->setWidget(dayList);
    dayPageLayout->addWidget(dayScroll, 1);
    m_stack->addWidget(dayPage);

    QWidget *monthPage = new QWidget;
    QVBoxLayout *monthPageLayout = new QVBoxLayout(monthPage);
    monthPageLayout->setContentsMargins(0, 0, 0, 0);
    monthPageLayout->setSpacing(8);
    QLabel *monthCaption = new QLabel(QStringLiteral("每日用时"));
    monthCaption->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(secondaryText()));
    monthPageLayout->addWidget(monthCaption);
    m_monthChart = new MonthlyChartWidget;
    monthPageLayout->addWidget(m_monthChart);
    QWidget *monthList = new QWidget;
    m_monthLayout = new QVBoxLayout(monthList);
    m_monthLayout->setContentsMargins(0, 0, 0, 0);
    m_monthLayout->setSpacing(0);
    QScrollArea *monthScroll = new QScrollArea;
    monthScroll->setWidgetResizable(true);
    monthScroll->setFrameShape(QFrame::NoFrame);
    monthScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    monthScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    monthScroll->setStyleSheet(QStringLiteral("QScrollArea{background:transparent;} QScrollArea>QWidget>QWidget{background:transparent;}"));
    monthScroll->viewport()->setAutoFillBackground(false);
    monthScroll->setWidget(monthList);
    monthPageLayout->addWidget(monthScroll, 1);
    m_stack->addWidget(monthPage);

    updateTitle();
    reload();
}

void UsagePopupWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    const QDate today = QDate::currentDate();
    m_date = (m_mode == Mode::Day) ? today : QDate(today.year(), today.month(), 1);
    emit opened();
}

void UsagePopupWidget::reload()
{
    if (m_mode == Mode::Day) {
        rebuildDayView();
    } else {
        rebuildMonthView();
    }
    if (m_stack)
        m_stack->setCurrentIndex(m_mode == Mode::Day ? 0 : 1);
    updateTitle();
    updateNavState();
}

QSize UsagePopupWidget::sizeHint() const
{
    // 列表放在滚动区里，内容高度不影响 hint；显式给一个更高的默认高度
    return QSize(360, 700);
}

void UsagePopupWidget::toggleMode()
{
    if (m_mode == Mode::Day) {
        // 进月视图前记住当前这一天，切回来时回到原来的日子
        m_dayBeforeMonth = m_date;
        m_mode = Mode::Month;
        m_date = QDate(m_date.year(), m_date.month(), 1);
    } else {
        m_mode = Mode::Day;
        const QDate today = QDate::currentDate();
        if (m_dayBeforeMonth.isValid() && m_dayBeforeMonth.year() == m_date.year()
            && m_dayBeforeMonth.month() == m_date.month()) {
            // 同一个月：回到进月视图前看的那一天
            m_date = m_dayBeforeMonth;
        } else if (m_date.year() == today.year() && m_date.month() == today.month()) {
            // 看的就是本月：直接回到今天
            m_date = today;
        }
        // 看的是别的月份：就停在该月 1 号
    }
    m_modeBtn->setText(m_mode == Mode::Day ? QStringLiteral("月视图") : QStringLiteral("日视图"));
    reload();
}

void UsagePopupWidget::prev()
{
    if (m_mode == Mode::Day)
        m_date = m_date.addDays(-1);
    else
        m_date = QDate(m_date.year(), m_date.month(), 1).addMonths(-1);
    reload();
}

void UsagePopupWidget::next()
{
    if (m_mode == Mode::Day)
        m_date = m_date.addDays(1);
    else
        m_date = QDate(m_date.year(), m_date.month(), 1).addMonths(1);
    reload();
}

void UsagePopupWidget::goToday()
{
    const QDate today = QDate::currentDate();
    if (m_mode == Mode::Day) {
        m_date = today;
    } else {
        m_date = QDate(today.year(), today.month(), 1);
        m_dayBeforeMonth = today; // 回日视图时直接停在今天
    }
    reload();
}

void UsagePopupWidget::updateTitle()
{
    if (m_mode == Mode::Day) {
        const QDate today = QDate::currentDate();
        const QString suffix = (m_date == today) ? QStringLiteral(" · 今天") : QString();
        m_title->setText(m_date.toString(QStringLiteral("yyyy-MM-dd ddd")) + suffix);
    } else {
        m_title->setText(m_date.toString(QStringLiteral("yyyy年M月")));
    }
}

void UsagePopupWidget::setChartMode(int mode)
{
    if (m_hourChart) {
        switch (mode) {
        case 1:
            m_hourChart->setMode(HourlyChartWidget::Mode::Awake);
            break;
        case 2:
            m_hourChart->setMode(HourlyChartWidget::Mode::Line);
            break;
        default:
            m_hourChart->setMode(HourlyChartWidget::Mode::Stacked);
            break;
        }
    }
    if (m_stackedBtn)
        m_stackedBtn->setChecked(mode == 0);
    if (m_awakeBtn)
        m_awakeBtn->setChecked(mode == 1);
    if (m_lineBtn)
        m_lineBtn->setChecked(mode == 2);
}

void UsagePopupWidget::updateNavState()
{
    if (m_mode == Mode::Day) {
        m_nextBtn->setEnabled(m_date < QDate::currentDate());
    } else {
        const QDate today = QDate::currentDate();
        m_nextBtn->setEnabled(QDate(m_date.year(), m_date.month(), 1) < QDate(today.year(), today.month(), 1));
    }
}

void UsagePopupWidget::rebuildDayView()
{
    clearLayout(m_dayLayout);

    const QString dateStr = m_date.toString(QStringLiteral("yyyy-MM-dd"));
    const QVector<AppUsageRow> rows = m_storage->queryDay(dateStr);

    if (m_hourChart) {
        const QVector<HourlyUsageRow> hourly = m_storage->queryHourlyRange(dateStr, dateStr);
        m_hourChart->setRows(hourly);
    }

    qint64 totalActive = 0;
    qint64 totalViewing = 0;
    for (const auto &r : rows) {
        totalActive += r.activeMs;
        totalViewing += r.viewingMs;
    }
    m_summary->setText(QStringLiteral("共使用 %1 · 操作 %2 · 查看 %3")
                           .arg(formatShort(totalActive + totalViewing))
                           .arg(formatShort(totalActive))
                           .arg(formatShort(totalViewing)));
    m_lockLabel->setText(QStringLiteral("锁屏时间 %1")
                             .arg(formatShort(m_storage->queryLock(dateStr))));

    if (rows.isEmpty()) {
        QLabel *empty = new QLabel(QStringLiteral("这一天没有使用记录"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(QStringLiteral("color:%1; padding:40px 0;").arg(secondaryText()));
        m_dayLayout->addWidget(empty);
    } else {
        for (const auto &r : rows)
            m_dayLayout->addWidget(new AppRowWidget(r));
    }
    m_dayLayout->addStretch();
}

void UsagePopupWidget::rebuildMonthView()
{
    clearLayout(m_monthLayout);

    const QString ym = m_date.toString(QStringLiteral("yyyy-MM"));
    const QVector<AppUsageRow> rows = m_storage->queryMonth(ym);

    qint64 totalActive = 0;
    qint64 totalViewing = 0;
    for (const auto &r : rows) {
        totalActive += r.activeMs;
        totalViewing += r.viewingMs;
    }
    m_summary->setText(QStringLiteral("本月共使用 %1 · 操作 %2 · 查看 %3")
                           .arg(formatShort(totalActive + totalViewing))
                           .arg(formatShort(totalActive))
                           .arg(formatShort(totalViewing)));
    m_lockLabel->setText(QStringLiteral("锁屏时间 %1")
                             .arg(formatShort(m_storage->queryLockMonth(ym))));

    const QDate today = QDate::currentDate();
    const int todayDay = (today.year() == m_date.year() && today.month() == m_date.month())
                             ? today.day()
                             : -1;
    if (m_monthChart)
        m_monthChart->setData(m_storage->queryMonthDailyTotals(ym), m_date.daysInMonth(), todayDay);

    if (rows.isEmpty()) {
        // 图表区已经写了「本月暂无记录」，这里不再重复一句
        m_monthLayout->addStretch();
        return;
    }
    for (const auto &r : rows)
        m_monthLayout->addWidget(new AppRowWidget(r));
    m_monthLayout->addStretch();
}

void UsagePopupWidget::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("关于"));
    box.setIcon(QMessageBox::NoIcon);
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral("<b>应用使用时间</b> " APP_USAGE_VERSION "<br><br>"
                               "作者：shao-jun<br>"
                               "GitHub：<a href=\"https://github.com/shao-jun/monitor\">"
                               "github.com/shao-jun/monitor</a>"));
    box.setStandardButtons(QMessageBox::Ok);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("关闭"));
    for (QLabel *label : box.findChildren<QLabel *>()) {
        label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        label->setOpenExternalLinks(true);
    }
    box.exec();
}
