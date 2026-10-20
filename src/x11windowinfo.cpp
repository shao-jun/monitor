#include "x11windowinfo.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <cstring>

// 忽略 X 错误（例如窗口在读取间隙被销毁导致的 BadWindow），避免进程被默认错误处理器终止
static int x11ErrorHandler(Display *, XErrorEvent *)
{
    return 0;
}

X11WindowInfo::X11WindowInfo() {}

X11WindowInfo::~X11WindowInfo()
{
    if (m_display)
        XCloseDisplay(m_display);
}

bool X11WindowInfo::init()
{
    XSetErrorHandler(x11ErrorHandler);
    m_display = XOpenDisplay(nullptr);
    if (!m_display)
        return false;

    m_netActiveWindow = XInternAtom(m_display, "_NET_ACTIVE_WINDOW", False);
    m_netWmPid = XInternAtom(m_display, "_NET_WM_PID", False);
    m_netWmName = XInternAtom(m_display, "_NET_WM_NAME", False);
    m_wmClass = XInternAtom(m_display, "WM_CLASS", False);
    m_utf8String = XInternAtom(m_display, "UTF8_STRING", False);
    m_ok = true;
    return true;
}

QString X11WindowInfo::readStringProperty(Window win, Atom atom) const
{
    Atom type;
    int fmt;
    unsigned long n = 0;
    unsigned long after = 0;
    unsigned char *data = nullptr;
    QString result;

    if (XGetWindowProperty(m_display, win, atom, 0, 1024, False, AnyPropertyType,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        if (type == XA_STRING || type == m_utf8String) {
            const char *p = reinterpret_cast<const char *>(data);
            result = QString::fromUtf8(p, static_cast<int>(n));
            int nul = result.indexOf(QChar('\0'));
            if (nul >= 0)
                result = result.left(nul);
        }
        XFree(data);
    }
    return result;
}

void X11WindowInfo::readWmClass(Window win, QString &instance, QString &klass) const
{
    Atom type;
    int fmt;
    unsigned long n = 0;
    unsigned long after = 0;
    unsigned char *data = nullptr;
    instance.clear();
    klass.clear();

    if (XGetWindowProperty(m_display, win, m_wmClass, 0, 1024, False, AnyPropertyType,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        if (n > 0) {
            const char *p = reinterpret_cast<const char *>(data);
            size_t firstLen = strnlen(p, static_cast<size_t>(n));
            instance = QString::fromLatin1(p, static_cast<int>(firstLen));
            size_t used = firstLen + 1;
            if (used < static_cast<size_t>(n)) {
                const char *p2 = p + used;
                size_t secondLen = strnlen(p2, static_cast<size_t>(n) - used);
                klass = QString::fromLatin1(p2, static_cast<int>(secondLen));
            }
        }
        XFree(data);
    }
}

X11WindowInfo::ActiveApp X11WindowInfo::activeApp() const
{
    ActiveApp a;
    if (!m_ok)
        return a;

    Window root = DefaultRootWindow(m_display);

    // 读取根窗口上的 _NET_ACTIVE_WINDOW
    Atom type;
    int fmt;
    unsigned long n = 0;
    unsigned long after = 0;
    unsigned char *data = nullptr;
    Window active = 0;

    if (XGetWindowProperty(m_display, root, m_netActiveWindow, 0, 1, False, XA_WINDOW,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        if (n >= 1)
            active = *reinterpret_cast<Window *>(data);
        XFree(data);
    }

    if (active == 0)
        return a; // 无聚焦窗口

    // 读取 _NET_WM_PID
    unsigned long pid = 0;
    data = nullptr;
    if (XGetWindowProperty(m_display, active, m_netWmPid, 0, 1, False, XA_CARDINAL,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        if (n >= 1)
            pid = *reinterpret_cast<unsigned long *>(data);
        XFree(data);
    }

    a.pid = static_cast<pid_t>(pid);
    readWmClass(active, a.wmClassInstance, a.wmClass);
    a.wmName = readStringProperty(active, m_netWmName);

    a.valid = (a.pid > 0) || !a.wmClassInstance.isEmpty() || !a.wmName.isEmpty();
    return a;
}
