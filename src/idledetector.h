#ifndef IDLEDETECTOR_H
#define IDLEDETECTOR_H

#include <QObject>
#include <QProcess>

// 通过 xprintidle（XScreenSaver 扩展）获取自上次用户输入以来的空闲毫秒数
class IdleDetector : public QObject
{
    Q_OBJECT
public:
    explicit IdleDetector(QObject *parent = nullptr);

    // 返回空闲毫秒数；失败返回 0（视为用户正在操作）
    long idleMilliseconds();

private:
    QProcess m_process;
};

#endif // IDLEDETECTOR_H
