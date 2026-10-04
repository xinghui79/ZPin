#include "logging.hpp"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QTextStream>
#include <QtGlobal>

#include "config.hpp"

namespace zpin::log {

namespace {

QFile* g_file = nullptr;
bool g_debug = false;
QtMessageHandler g_prev = nullptr;
// 写盘 / 历史压 PNG / 特效预热这些后台线程都会直接写日志，共用同一个 QFile，
// 不串行化会行交错甚至互相覆盖。
QMutex g_logMutex;

const char* levelName(QtMsgType type) {
    switch (type) {
        case QtDebugMsg: return "DEBUG";
        case QtInfoMsg: return "INFO";
        case QtWarningMsg: return "WARNING";
        case QtCriticalMsg: return "ERROR";
        default: return "FATAL";
    }
}

void writeLine(QTextStream& out, const char* level, const QString& category,
               const QString& text) {
    // 类目统一收进 "zpin.xxx" 风格，Rust 侧的 panic 行也按同一形状输出
    QString name = category.isEmpty() ? QStringLiteral("zpin") : category;
    if (!name.startsWith(QLatin1String("zpin")))
        name.prepend(QLatin1String("zpin."));
    out << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << " [" << level << "] "
        << name << ": " << text << '\n';
}

void emitLine(const char* level, const QString& category, const QString& text) {
    QMutexLocker lock(&g_logMutex);
    // 必须判 isOpen()，不能只判 g_file 非空：open 失败时 g_file 仍然非空
    // （init() 建了就没再管过），此后全链路日志——包括 main.cpp 装的
    // set_terminate 兜底——会被无声吞掉，排障时连「为什么没日志」都看不到
    if (!g_file || !g_file->isOpen())
        return;
    QTextStream out(g_file);   // Qt6 默认即 UTF-8
    writeLine(out, level, category, text);
    out.flush();
}

void qtHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& text) {
    const bool isError = type == QtCriticalMsg || type == QtFatalMsg;
    const bool showOnStderr = type != QtDebugMsg || isError;
    if (type == QtDebugMsg && !g_debug)
        return;  // 调试级且未开详细模式：直接丢，isError 在这里恒为假
    // 出处补不了：实测 Qt 6.11.3 官方 msvc 包里 qWarning 的 ctx.file/function
    // 恒为空（Qt 发布版编译时定义了 QT_NO_MESSAGELOGCONTEXT，编译期就省掉了
    // __FILE__/__func__）。所以「QFont::setPointSize: Point size <= 0 (-1)」这类
    // Qt 内部告警只能看到一句话，想知道谁触发的得抓调用栈，成本不值。
    emitLine(levelName(type), ctx.category ? QString::fromLatin1(ctx.category) : QString(), text);
    if (showOnStderr) {
        QTextStream err(stderr, QIODevice::WriteOnly);
        writeLine(err, levelName(type),
                  ctx.category ? QString::fromLatin1(ctx.category) : QString(), text);
    }
    if (isError && g_prev)
        g_prev(type, ctx, text);
}

}  // namespace

void init(bool debug) {
    g_debug = debug;
    if (!g_file) {
        QFile* f = new QFile(config::appDir() + "/zpin.log");
        // 轮转：常驻托盘进程无限追加，日志会滚到几百 MB。超 5MB 就把上一份
        // 改名 .old（只留一代，排错要看的一般就是最近这轮）后重开。改名失败
        // （文件被占等）就照旧追加，不为此断日志。
        constexpr qint64 kMaxLogBytes = 5 * 1024 * 1024;
        if (f->size() > kMaxLogBytes) {
            const QString name = f->fileName();
            QFile::remove(name + ".old");
            f->rename(name + ".old");
        }
        if (f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            g_file = f;
        } else {
            // 打不开就彻底没日志了（含 set_terminate 兜底），至少往 stderr 交代一句，
            // 否则磁盘满/权限被改时用户和开发者都拿不到任何线索
            QTextStream err(stderr, QIODevice::WriteOnly);
            err << "ZPin: 日志文件无法打开（" << f->fileName() << "）："
                << f->errorString() << '\n';
            delete f;   // init() 在事件循环起来之前调用，deleteLater 不会跑
        }
    }
    // 只记一次原始 handler：再装一遍会让 qtHandler 链到自己，错误日志无限递归
    if (!g_prev)
        g_prev = qInstallMessageHandler(qtHandler);
    log::info("zpin", "日志就绪");
}

void setDebug(bool debug) {
    g_debug = debug;
}

void debug(const char* category, const QString& text) {
    if (g_debug)
        emitLine("DEBUG", category, text);
}

void info(const char* category, const QString& text) {
    emitLine("INFO", category, text);
}

void warn(const char* category, const QString& text) {
    emitLine("WARNING", category, text);
}

void error(const char* category, const QString& text) {
    emitLine("ERROR", category, text);
}

}  // namespace zpin::log
