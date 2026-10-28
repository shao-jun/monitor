#ifndef X11WINDOWINFO_H
#define X11WINDOWINFO_H

#include <QtGlobal>
#include <QString>

#include <X11/Xlib.h>

// 通过 X11 读取当前处于最上层（获得焦点）的窗口及其归属进程信息
class X11WindowInfo
{
public:
    struct ActiveApp {
        pid_t pid = 0;
        QString wmClassInstance; // WM_CLASS 第一段（应用类名）
        QString wmClass;         // WM_CLASS 第二段
        QString wmName;          // _NET_WM_NAME（窗口标题）
        unsigned long window = 0; // 聚焦窗口的 X11 id
        bool valid = false;      // false 表示当前没有聚焦窗口（桌面）
    };

    X11WindowInfo();
    ~X11WindowInfo();

    bool init();
    ActiveApp activeApp() const;

    // 取窗口在屏幕上的绝对位置与大小
    bool windowRect(unsigned long win, int &x, int &y, int &width, int &height) const;

private:
    QString readStringProperty(Window win, Atom atom) const;
    void readWmClass(Window win, QString &instance, QString &klass) const;

    Display *m_display = nullptr;
    Atom m_netActiveWindow = 0;
    Atom m_netWmPid = 0;
    Atom m_netWmName = 0;
    Atom m_wmClass = 0;
    Atom m_utf8String = 0;
    bool m_ok = false;
};

#endif // X11WINDOWINFO_H
