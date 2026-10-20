#include "idledetector.h"

IdleDetector::IdleDetector(QObject *parent)
    : QObject(parent)
{
}

long IdleDetector::idleMilliseconds()
{
    m_process.start(QStringLiteral("xprintidle"));
    if (!m_process.waitForFinished(500)) {
        m_process.kill();
        m_process.waitForFinished(100);
        return 0;
    }

    const QByteArray out = m_process.readAllStandardOutput().trimmed();
    bool ok = false;
    long v = out.toLong(&ok);
    return ok ? v : 0;
}
