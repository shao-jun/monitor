#ifndef USAGEPOPUPWIDGET_H
#define USAGEPOPUPWIDGET_H

#include <QDate>
#include <QWidget>

class UsageStorage;
class HourlyChartWidget;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

class UsagePopupWidget : public QWidget
{
    Q_OBJECT
public:
    explicit UsagePopupWidget(UsageStorage *storage, QWidget *parent = nullptr);

public slots:
    void reload();

signals:
    void opened();

public:
    QSize sizeHint() const override;

protected:
    void showEvent(QShowEvent *e) override;

private:
    enum class Mode { Day, Month };

    void prev();
    void next();
    void goToday();
    void toggleMode();
    void updateTitle();
    void updateNavState();
    void rebuildDayView();
    void rebuildMonthView();
    void setChartMode(int mode);

    UsageStorage *m_storage;
    QDate m_date;
    Mode m_mode = Mode::Day;

    QLabel *m_title = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_lockLabel = nullptr;
    QPushButton *m_prevBtn = nullptr;
    QPushButton *m_nextBtn = nullptr;
    QPushButton *m_todayBtn = nullptr;
    QPushButton *m_modeBtn = nullptr;
    QStackedWidget *m_stack = nullptr;
    HourlyChartWidget *m_hourChart = nullptr;
    QPushButton *m_stackedBtn = nullptr;
    QPushButton *m_awakeBtn = nullptr;
    QPushButton *m_lineBtn = nullptr;
    QVBoxLayout *m_dayLayout = nullptr;
    QVBoxLayout *m_monthLayout = nullptr;
};

#endif // USAGEPOPUPWIDGET_H
