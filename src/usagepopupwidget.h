#ifndef USAGEPOPUPWIDGET_H
#define USAGEPOPUPWIDGET_H

#include <QDate>
#include <QWidget>

class UsageStorage;
class HourlyChartWidget;
class MonthlyChartWidget;
class SwitchChartWidget;
class MonthlyChartWidget;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

class UsagePopupWidget : public QWidget
{
    Q_OBJECT
public:
    explicit UsagePopupWidget(UsageStorage *storage, QWidget *parent = nullptr);

    bool isShowingSwitches() const { return m_mode == Mode::Switch; }

public slots:
    void reload();

signals:
    void opened();

public:
    QSize sizeHint() const override;

protected:
    void showEvent(QShowEvent *e) override;

private:
    enum class Mode { Day, Month, Switch };

    void prev();
    void next();
    void goToday();
    void toggleMode();
    void toggleSwitchView();
    void updateHeaderButtons();
    void updateTitle();
    void updateNavState();
    void rebuildDayView();
    void rebuildMonthView();
    void rebuildSwitchView();
    void addEvent();
    void setChartMode(int mode);
    void showAbout();

    UsageStorage *m_storage;
    QDate m_date;
    QDate m_dayBeforeMonth; // 从日视图切到月视图前停留的那一天
    Mode m_mode = Mode::Day;

    QLabel *m_title = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_lockLabel = nullptr;
    QPushButton *m_prevBtn = nullptr;
    QPushButton *m_nextBtn = nullptr;
    QPushButton *m_todayBtn = nullptr;
    QPushButton *m_modeBtn = nullptr;
    QPushButton *m_switchBtn = nullptr;
    QPushButton *m_aboutBtn = nullptr;
    QStackedWidget *m_stack = nullptr;
    HourlyChartWidget *m_hourChart = nullptr;
    MonthlyChartWidget *m_monthChart = nullptr;
    QPushButton *m_stackedBtn = nullptr;
    QPushButton *m_awakeBtn = nullptr;
    QPushButton *m_lineBtn = nullptr;
    QVBoxLayout *m_dayLayout = nullptr;
    QVBoxLayout *m_monthLayout = nullptr;
    QLabel *m_switchSummary = nullptr;
    SwitchChartWidget *m_switchChart = nullptr;
    QVBoxLayout *m_switchLayout = nullptr;
};

#endif // USAGEPOPUPWIDGET_H
