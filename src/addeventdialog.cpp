#include "addeventdialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimeEdit>
#include <QVBoxLayout>

AddEventDialog::AddEventDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("添加事件"));
    setMinimumWidth(360);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 12);
    root->setSpacing(10);

    QFormLayout *form = new QFormLayout;
    form->setSpacing(8);

    m_label = new QLineEdit;
    m_label->setPlaceholderText(QStringLiteral("比如：出去开会"));
    form->addRow(QStringLiteral("事件"), m_label);

    const QDateTime now = QDateTime::currentDateTime();
    m_start = new QTimeEdit(now.addSecs(-15 * 60).time());
    m_start->setDisplayFormat(QStringLiteral("HH:mm"));
    form->addRow(QStringLiteral("开始"), m_start);

    m_minutes = new QSpinBox;
    m_minutes->setRange(1, 600);
    m_minutes->setValue(30);
    m_minutes->setSuffix(QStringLiteral(" 分钟"));
    form->addRow(QStringLiteral("时长"), m_minutes);

    root->addLayout(form);

    // 常用事件，一点就填好
    QHBoxLayout *chips = new QHBoxLayout;
    chips->setSpacing(4);
    const QStringList presets{QStringLiteral("出去开会"), QStringLiteral("被人叫走"),
                              QStringLiteral("接电话"), QStringLiteral("休息")};
    for (const QString &text : presets) {
        QPushButton *b = new QPushButton(text);
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, text]() { m_label->setText(text); });
        chips->addWidget(b);
    }
    chips->addStretch();
    root->addLayout(chips);

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setText(QStringLiteral("添加"));
    box->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(box, &QDialogButtonBox::accepted, this, [this]() {
        if (m_label->text().trimmed().isEmpty())
            m_label->setText(QStringLiteral("事件"));
        accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(box);
}

QString AddEventDialog::label() const
{
    const QString text = m_label->text().trimmed();
    return text.isEmpty() ? QStringLiteral("事件") : text;
}

QTime AddEventDialog::startTime() const
{
    return m_start->time();
}

int AddEventDialog::minutes() const
{
    return m_minutes->value();
}

qint64 AddEventDialog::startTs() const
{
    const QDate today = QDate::currentDate();
    return QDateTime(today, m_start->time()).toSecsSinceEpoch();
}

qint64 AddEventDialog::endTs() const
{
    qint64 end = startTs() + qint64(m_minutes->value()) * 60;
    const qint64 dayEnd = QDateTime(QDate::currentDate(), QTime(23, 59, 59)).toSecsSinceEpoch();
    return qMin(end, dayEnd);
}
