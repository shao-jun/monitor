#ifndef ADDEVENTDIALOG_H
#define ADDEVENTDIALOG_H

#include <QDialog>
#include <QTime>

class QLineEdit;
class QSpinBox;
class QTimeEdit;

// 手动补一条事件：出去开会、被人叫走、接电话……
class AddEventDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AddEventDialog(QWidget *parent = nullptr);

    QString label() const;
    QTime startTime() const;
    int minutes() const;
    qint64 startTs() const;
    qint64 endTs() const;

private:
    QLineEdit *m_label = nullptr;
    QTimeEdit *m_start = nullptr;
    QSpinBox *m_minutes = nullptr;
};

#endif // ADDEVENTDIALOG_H
