#include "ocr.hpp"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QMimeData>
#include <QRegularExpression>
#include <QRunnable>
#include <QStringList>
#include <QThreadPool>
#include <QtGlobal>

#include "logging.hpp"
#include "rcore.hpp"

namespace zpin::ocr {
namespace {

template <typename Result>
class Task : public QRunnable {
public:
    Task(const QImage& img, std::function<void(Result)> onDone,
         Result (*work)(const QImage&))
        : m_img(img), m_onDone(std::move(onDone)), m_work(work) {}

    void run() override {
        const Result result = m_work(m_img);
        // 工作线程不碰界面：结果投递回主线程（qApp 住主线程）
        QMetaObject::invokeMethod(qApp, [done = m_onDone, result] { done(result); },
                                  Qt::QueuedConnection);
    }

private:
    QImage m_img;
    std::function<void(Result)> m_onDone;
    Result (*m_work)(const QImage&);
};

// 表格识别的耗时埋点：这条比文字识别重得多（版面一趟 + 结构一趟 + 单元格识别一趟），
// 没有实测数字就没法判断要不要挪去独立进程或加 DirectML（AGENTS.md 第 2 条）。
std::optional<QVector<rcore::OcrTable>> recognizeTablesTimed(const QImage& img) {
    QElapsedTimer clock;
    clock.start();
    std::optional<QVector<rcore::OcrTable>> tables = rcore::ocrTables(img);
    const qsizetype count = tables ? qsizetype(tables->size()) : qsizetype(-1);
    log::info("zpin.table",
              QStringLiteral("耗时 %1ms，命中 %2 张表").arg(clock.elapsed()).arg(count));
    return tables;
}

}  // namespace

void recognizeAsync(const QImage& img, std::function<void(std::optional<QString>)> onDone) {
    QThreadPool::globalInstance()->start(
        new Task<std::optional<QString>>(img, std::move(onDone), rcore::ocrText));
}

void recognizeBoxesAsync(
    const QImage& img, std::function<void(std::optional<QVector<rcore::OcrBox>>)> onDone) {
    QThreadPool::globalInstance()->start(
        new Task<std::optional<QVector<rcore::OcrBox>>>(img, std::move(onDone),
                                                        rcore::ocrBoxes));
}

void recognizeTablesAsync(
    const QImage& img, std::function<void(std::optional<QVector<rcore::OcrTable>>)> onDone) {
    QThreadPool::globalInstance()->start(
        new Task<std::optional<QVector<rcore::OcrTable>>>(img, std::move(onDone),
                                                          recognizeTablesTimed));
}

bool publishText(const std::optional<QString>& text,
                 const std::function<void(QString, QString)>& notify) {
    const QString title = QStringLiteral("识别文字");
    if (!text) {
        notify(title, QStringLiteral("系统 OCR 不可用或识别失败（详见日志）"));
        return false;
    }
    const QString trimmed = text->trimmed();
    if (trimmed.isEmpty()) {
        notify(title, QStringLiteral("没有识别到文字"));
        return false;
    }
    QApplication::clipboard()->setText(trimmed);
    notify(title, QStringLiteral("已复制 %1 段文字到剪贴板")
                      .arg(trimmed.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size()));
    return true;
}

bool publishTables(const std::optional<QVector<rcore::OcrTable>>& tables,
                   const std::function<void(QString, QString)>& notify) {
    const QString title = QStringLiteral("识别表格");
    if (!tables) {
        notify(title, QStringLiteral("表格识别不可用（详见日志）"));
        return false;
    }
    if (tables->isEmpty()) {
        notify(title, QStringLiteral("没有识别到表格"));
        return false;
    }
    QStringList plain;
    // Rust 只给 <table> 片段，文档外壳在这里包一次：两张表各带一份 <html><body>
    // 拼起来是非法 HTML，CF_HTML 的偏移会算歪，Excel 只认第一张
    QString html = QStringLiteral("<html><body>");
    for (const rcore::OcrTable& t : *tables) {
        html += t.html;
        plain << t.text;
    }
    html += QStringLiteral("</body></html>");
    auto* mime = new QMimeData();
    mime->setHtml(html);
    mime->setText(plain.join(QStringLiteral("\n\n")));
    QApplication::clipboard()->setMimeData(mime);
    notify(title, QStringLiteral("已复制 %1 个表格（可粘进 Excel/WPS）").arg(tables->size()));
    return true;
}

bool looksSensitiveText(const QString& text) {
    // OCR 常把 @ 识别成全角＠或在两侧插空格：先归一化再匹配
    QString normalized = text;
    normalized.replace(u'＠', u'@');
    static const QRegularExpression kPhone(QStringLiteral("1[3-9]\\d{9}"));   // 手机号
    static const QRegularExpression kMail(QStringLiteral(
        "[A-Za-z0-9._%+-]+\\s*[@]\\s*[A-Za-z0-9.-]+\\s*\\.\\s*[A-Za-z]{2,}"));  // 邮箱
    static const QRegularExpression kIdCard(QStringLiteral("\\d{17}[\\dXx]"));  // 身份证
    if (kPhone.match(normalized).hasMatch() || kIdCard.match(normalized).hasMatch())
        return true;
    return kMail.match(normalized).hasMatch();
}

}  // namespace zpin::ocr
