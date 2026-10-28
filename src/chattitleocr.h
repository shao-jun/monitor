#ifndef CHATTITLEOCR_H
#define CHATTITLEOCR_H

#include <QImage>
#include <QString>

class X11WindowInfo;

// 用截图 + tesseract 识别企业微信当前会话标题（Wine 客户端窗口标题恒定，只能靠认字）
class ChatTitleOcr
{
public:
    explicit ChatTitleOcr(X11WindowInfo *x11 = nullptr);

    void setX11(X11WindowInfo *x11) { m_x11 = x11; }
    bool available() const;
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on) { m_enabled = on; }

    // 抓取该窗口的“聊天标题”小条并识别；失败返回空串
    QString titleForWindow(unsigned long winId, const QString &appName);

private:
    static QString pickTitle(const QString &raw, const QString &appName);

    X11WindowInfo *m_x11 = nullptr;
    bool m_enabled = true;
    mutable int m_available = -1; // -1 未知，0 没有，1 有
    int m_goodLayout = -1;        // 上次认出标题的布局序号，优先复用
};

#endif // CHATTITLEOCR_H
