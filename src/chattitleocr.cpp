#include "chattitleocr.h"
#include <QFileInfo>
#include <QDateTime>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPixmap>
#include <QRect>
#include <QVector>
#include <QProcess>
#include <QRegExp>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryFile>

// X11 宏会污染 Qt 头文件，放在最后
#include "x11windowinfo.h"

namespace {

// 可疑识别（短纯字母）留证：~/.cache/app-usage-monitor/ocr/ 下留最近 20 张条带图
QString suspectDir()
{
    const QString dir = QDir::home().filePath(QStringLiteral(".cache/app-usage-monitor/ocr"));
    QDir().mkpath(dir);
    return dir;
}

QString suspectPath(const QString &title)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QRegExp bad(QStringLiteral("[^A-Za-z0-9]"));
    const QString safe = QString(title).replace(bad, QStringLiteral("_"));
    return QDir(suspectDir()).filePath(QStringLiteral("%1-%2.png").arg(stamp, safe));
}

void saveSuspectText(const QString &raw)
{
    QFile f(QDir(suspectDir()).filePath(QStringLiteral("last-raw.txt")));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(raw.toUtf8());

    QDir dir(suspectDir());
    const QFileInfoList list = dir.entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Name);
    for (int i = 0; i < list.size() - 20; ++i)
        QFile::remove(list.at(i).absoluteFilePath());
}

} // namespace

ChatTitleOcr::ChatTitleOcr(X11WindowInfo *x11)
    : m_x11(x11)
{
}

bool ChatTitleOcr::available() const
{
    if (m_available < 0)
        m_available = QStandardPaths::findExecutable(QStringLiteral("tesseract")).isEmpty() ? 0 : 1;
    return m_available == 1;
}

// 企业微信的布局和窗口大小有关，标题条位置不固定：按几套已知布局依次试，
// 哪套认出来了就记住，下次先试它。
QString ChatTitleOcr::titleForWindow(unsigned long winId, const QString &appName)
{
    if (!m_enabled || !m_x11 || !available())
        return QString();

    int x = 0, y = 0, w = 0, h = 0;
    if (!m_x11->windowRect(winId, x, y, w, h))
        return QString();
    if (w < 600 || h < 400)
        return QString();

    QVector<QRect> strips;
    // 1) 最大化窗口：会话列表宽度固定，标题从窗口左边 +404 开始
    strips.append(QRect(x + 404, y + 24, qBound(200, w - 480, 760), 36));
    // 2) 窄一些的布局：面板起点更靠右
    strips.append(QRect(x + 484, y + 26, qBound(180, w - 560, 520), 32));
    // 3) 按比例兜底（窗口被拖成非整数倍缩放时）
    strips.append(QRect(x + int(w * 0.21), y + int(h * 0.03), qBound(180, int(w * 0.6), 700), 36));

    if (m_goodLayout > 0 && m_goodLayout < strips.size())
        std::swap(strips[0], strips[m_goodLayout]);

    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QString();

    for (int i = 0; i < strips.size(); ++i) {
        const QRect &strip = strips.at(i);
        if (strip.width() < 100)
            continue;

        QImage img = screen->grabWindow(0, strip.x(), strip.y(), strip.width(), strip.height()).toImage();
        if (img.isNull() || img.width() < 80)
            continue;

        // 小字号中文：放大 4 倍再拉伸对比度，否则 tesseract 基本认不出
        img = img.convertToFormat(QImage::Format_Grayscale8);
        img = img.scaled(img.width() * 4, img.height() * 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        int lo = 255, hi = 0;
        for (int yy = 0; yy < img.height(); ++yy) {
            const uchar *line = img.constScanLine(yy);
            for (int xx = 0; xx < img.width(); ++xx) {
                lo = qMin(lo, int(line[xx]));
                hi = qMax(hi, int(line[xx]));
            }
        }
        if (hi - lo < 20)
            continue; // 整条几乎纯色，没有文字

        for (int yy = 0; yy < img.height(); ++yy) {
            uchar *line = img.scanLine(yy);
            for (int xx = 0; xx < img.width(); ++xx)
                line[xx] = uchar(qBound(0, (int(line[xx]) - lo) * 255 / (hi - lo), 255));
        }

        QTemporaryFile file(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                .filePath(QStringLiteral("app-usage-chat-XXXXXX.png")));
        if (!file.open())
            continue;
        const QString path = file.fileName();
        file.close();
        if (!img.save(path, "PNG"))
            continue;

        // 先只用中文模型：chi_sim+eng 会把「吴博文」认成「RSI」这种拉丁垃圾，
        // 中文名才是主场景；中文认不出来再回退到混合模型（英文群名靠它）
        QString out;
        QString title;
        for (const QString &lang : {QStringLiteral("chi_sim"), QStringLiteral("chi_sim+eng")}) {
            QProcess proc;
            proc.start(QStringLiteral("tesseract"),
                       {path, QStringLiteral("-"), QStringLiteral("-l"), lang,
                        QStringLiteral("--psm"), QStringLiteral("6")});
            if (proc.waitForFinished(3000))
                out = QString::fromUtf8(proc.readAllStandardOutput());
            else
                proc.kill();
            title = pickTitle(out, appName);
            if (!title.isEmpty())
                break;
        }
        if (!title.isEmpty()) {
            // 只有拉丁字母、又特别短的（「ZAG」这种），八成是 OCR 认歪了，
            // 留一张条带图 + 原始文本，方便回头核对到底读到了什么
            bool latinOnly = !title.isEmpty();
            for (const QChar &c : title) {
                if (c.script() == QChar::Script_Han) {
                    latinOnly = false;
                    break;
                }
            }
            if (latinOnly && title.size() <= 5 && img.save(suspectPath(title), "PNG"))
                saveSuspectText(out);

            m_goodLayout = i;
            QFile::remove(path);
            return title;
        }
        QFile::remove(path);
    }
    return QString();
}

QString ChatTitleOcr::pickTitle(const QString &raw, const QString &appName)
{
    // 标题行是「会话名 + 下拉箭头 + 职位/状态」，右边那截是干扰，先按关键词切掉
    static const QStringList subtitles{
        QStringLiteral("研发经理"), QStringLiteral("经理"), QStringLiteral("工程师"),
        QStringLiteral("资深"), QStringLiteral("研发"), QStringLiteral("桌面"),
        QStringLiteral("主管"), QStringLiteral("总监"), QStringLiteral("专员"),
        QStringLiteral("助理"), QStringLiteral("组长"), QStringLiteral("专家"),
        QStringLiteral("实习生"), QStringLiteral("在线"), QStringLiteral("离线"),
        QStringLiteral("忙碌"), QStringLiteral("请勿打扰"),
    };

    const QStringList lines = raw.split(QLatin1Char('\n'), QString::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        int cut = line.size();
        bool roleFound = false;
        for (const QString &word : subtitles) {
            const int idx = line.indexOf(word);
            if (idx > 0 && idx < cut) {
                cut = idx;
                roleFound = true;
            }
        }
        QString title = line.left(cut).trimmed();

        const QStringList parts = title.split(QRegExp(QStringLiteral("\\s{2,}")), QString::SkipEmptyParts);
        if (!parts.isEmpty())
            title = parts.first().trimmed();

        // 去掉 OCR 常见噪声：箭头、引号、括号、句读，出现在中间也一并清掉
        // 注意别把【】（）这类括号当噪声：群名里全是它们
        static const QString junk = QStringLiteral("\t·|<>\u2324\u2039\u203a\"\u201c\u201d\u2018\u2019\uff0c,\u3001\uff1a\uff1b");
        for (int i = title.size() - 1; i >= 0; --i) {
            if (junk.contains(title.at(i)))
                title.remove(i, 1);
        }
        title = title.simplified();

        title = title.trimmed();

        const QStringList toks = title.split(QLatin1Char(' '), QString::SkipEmptyParts);
        if (roleFound) {
            // 标题行里带职位（资深桌面研发工程师…）说明是一对一私聊，会话名就是最前面那个词，
            // 后面那些是状态图标被 OCR 认出来的杂字
            if (!toks.isEmpty())
                title = toks.first();
        } else if (toks.size() >= 2) {
            // 群名：去掉尾部单字噪声（「DDE V20 拉」那种）
            QStringList tail = toks;
            while (tail.size() >= 2 && tail.last().size() <= 1) {
                title.chop(tail.last().size());
                tail.removeLast();
            }
            title = title.trimmed();
        }

        int han = 0;
        int latin = 0;
        for (const QChar &c : title) {
            if (c.script() == QChar::Script_Han)
                ++han;
            else if (c.isLetterOrNumber())
                ++latin;
        }
        // 纯拉丁的短串（ZAG、RSI 这种 3 个字母）基本都是把汉字认歪了，宁可当作没认出来
        if (han < 2) {
            if (latin < 4)
                continue;
            // 纯英文的会话名（DDE V20、Google Chrome）每个词都该大写或数字开头，
            // 「BS were 7 rr) eas」这种是小写英文句子，直接判为识别失败
            bool ok = true;
            for (const QString &tok : title.split(QLatin1Char(' '), QString::SkipEmptyParts)) {
                const QChar c0 = tok.at(0);
                if (!c0.isUpper() && !c0.isDigit()) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;
        }
        if (title.contains(QStringLiteral("企业微信")) || title == appName)
            continue;
        return title.left(60);
    }
    return QString();
}
