// ZPin 进程入口 —— 日志、单实例守卫、装配 App（app.cpp）并进入事件循环。
#include <QApplication>
#include <QFont>

#include <chrono>
#include <cstdlib>
#include <exception>

#include "app.hpp"
#include "config.hpp"
#include "defaults.hpp"
#include "hotkey.hpp"
#include "logging.hpp"
#include "ui_app_icon.hpp"
#include "win32util.hpp"

#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace {

qint64 elapsedMs(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

// 任何未捕获异常都要落进 zpin.log：窗口进程崩了没有控制台可看。
void installTerminateHandler() {
    std::set_terminate([] {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            zpin::log::error("zpin", QStringLiteral("未捕获异常: %1")
                                   .arg(QString::fromLocal8Bit(e.what())));
        } catch (...) {
            zpin::log::error("zpin", QStringLiteral("未捕获异常（非标准异常类型）"));
        }
        std::abort();
    });
}

// Qt 自带对话框（选颜色、另存为、消息框的标准按钮）的中文翻译。
// 不加载就是全英文——「选择颜色」弹窗里的 Custom colors / OK / Cancel、
// 另存为里的 File name / Files of type 全是英文，在中文界面里很突兀。
// 翻译文件由 build.bat 拷进 exe 同级的 translations\qtbase_zh_CN.qm。
// 逐个前缀试是因为不同 Qt 发行版拆分模块的方式不一样（有的把 QtWidgets 单独
// 拆成 qtwidgets_*.qm，有的合在 qtbase 里），构建期无法确定具体有哪些文件。
// 必须装进 main 里的 static：translator 早于 QApplication 析构被销毁就崩，
// 而 QApplication 是栈对象，main 作用域的局部变量会先死。
void installTranslations() {
    static QTranslator base;
    static QTranslator widgets;
    static QTranslator merged;   // 兜底：某些 Qt 发行版只有合并版
    const QLocale locale = QLocale::system();
    const QString dir = QLibraryInfo::path(QLibraryInfo::LibraryPath::TranslationsPath);
    int loaded = 0;
    if (base.load(locale, QStringLiteral("qtbase"), QStringLiteral("_"), dir)) {
        QCoreApplication::installTranslator(&base);
        ++loaded;
    }
    if (widgets.load(locale, QStringLiteral("qtwidgets"), QStringLiteral("_"), dir)) {
        QCoreApplication::installTranslator(&widgets);
        ++loaded;
    }
    if (loaded == 0 && merged.load(locale, QStringLiteral("qt"), QStringLiteral("_"), dir)) {
        QCoreApplication::installTranslator(&merged);
        loaded = 1;
        zpin::log::warn("zpin", QStringLiteral("Qt 翻译只找到合并版 qt_%1（非标准拆分），已改用它")
                                  .arg(locale.name()));
    }
    if (loaded == 0)
        zpin::log::warn("zpin", QStringLiteral("找不到 Qt 中文翻译（目录 %1），自带对话框将显示英文")
                                  .arg(dir));
}

}  // namespace

int main(int argc, char** argv) {
    const auto t0 = std::chrono::steady_clock::now();
    // 多屏混合 DPI：抓图画布就是物理虚拟桌面，每屏原生像素 1:1，逻辑<->物理
    // 换算由 capture::DesktopMap 分段自己做。Qt 6 恒定启用高 DPI 缩放，Qt 5 的
    // QT_AUTO_SCREEN_SCALE_FACTOR 开关在 Qt 6 是无效变量，不设。

    // 顺序：兜底处理器与日志必须排在 config::init() 之前。
    // 日志只依赖 config::appDir()（读环境变量，不碰 g_settings），所以能先开起来；
    // 反过来先跑 config::init() 的话，它内部的备份失败/热键迁移这些
    // log::warn/log::info 全打在 g_file 还为空的时候，被 emitLine 无声丢弃，
    // 配置真被迁移改坏时日志里一个字都没有。
    installTerminateHandler();
    zpin::log::init(false);
    zpin::config::init();
    zpin::log::setDebug(zpin::config::getStr(QStringLiteral("General/log_level"))
                        == QStringLiteral("详细"));
    zpin::log::info("zpin", QStringLiteral("启动[1/3] 日志与配置就绪 %1ms").arg(elapsedMs(t0)));

    // 单实例：已运行则让那个实例弹气泡，本进程退出
    if (!zpin::win32::acquireSingleInstance()) {
        zpin::hotkey::notifyShow();
        return 0;
    }

    QApplication app(argc, argv);
    installTranslations();
    app.setApplicationName(QStringLiteral("ZPin"));
    app.setWindowIcon(zpin::appicon::appIcon());
    // 关完窗口不退进程：托盘才是主界面
    app.setQuitOnLastWindowClosed(false);
    const auto [fontName, fontSize] =
        zpin::defaults::fontTuple(zpin::config::getStr(QStringLiteral("Interface/font")));
    // 与 App::applyFont 同一套：fontName 可能是「A,B,C」的字体栈，见
    // defaults::kDefaultFontFamily。启动时这里设一次，之后用户在设置页改字号/
    // 字体由 applyFont 接手
    app.setFont(QFont(fontName.split(QLatin1Char(','), Qt::SkipEmptyParts), fontSize));

    zpin::App zpinApp;
    zpin::log::info("zpin", QStringLiteral("启动[2/3] 子系统装配完成 %1ms").arg(elapsedMs(t0)));
    zpinApp.wire();
    zpin::log::info("zpin", QStringLiteral("启动[3/3] 信号接线完成 %1ms").arg(elapsedMs(t0)));
    return zpinApp.run();
}
