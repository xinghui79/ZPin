// 自动更新 —— GitHub Releases 的检查 / 下载 / 换装。
// 网络与解压都是同步的，一律经 QThreadPool 离开 UI 线程；UpdateFlow 是装配层
// （app.cpp）持有的状态机，设置 → 关于 的界面与启动气泡都只看它的信号。
#pragma once

#include <QObject>
#include <QString>

#include <optional>

class QTimer;

namespace zpin {
namespace update {

struct Release {
    QString version;      // 去掉 v 前缀的版本号，如 "1.4.0"
    QString downloadUrl;  // .zip 资产直链；发布没带 zip 时为空（只能去发布页手动下）
    QString digest;       // "sha256:…"（GitHub 资产摘要；为空时只按字节数校验）
    qint64 size = 0;      // .zip 资产字节数
};

// 拉取最新 release（同步，工作线程调用）；release 为空时 error 给出原因。
struct Latest {
    std::optional<Release> release;
    QString error;
};
Latest latestRelease();

// 语义版本比较：>0 a 更新，<0 b 更新，0 相同（逐段取数字，非数字后缀忽略）。
int compareVersions(const QString& a, const QString& b);

// 同步下载 release 资产 zip 到 dest；失败返回 false 并把原因写入 *error。
bool downloadTo(const Release& release, const QString& dest, QString* error);

// 解压 zip 到 dir（系统自带 tar），返回内容根目录（zip 内容若包在单一子文件夹
// 里则返回那个子文件夹）；失败返回空串并把原因写入 *error。
QString extractZip(const QString& zip, const QString& dir, QString* error);

// 生成换装脚本并启动：等本进程退出 -> 覆盖安装目录 -> 重启新 exe -> 清理临时
// 文件。sourceDir 是新版本内容根；workDir 与 zip 是脚本收尾的清理对象。
void startInstaller(const QString& sourceDir, const QString& workDir, const QString& zip);

// ---- 状态机：检查 -> 发现新版本 -> 下载 -> 就绪换装 ----
class UpdateFlow : public QObject {
    Q_OBJECT

public:
    enum class State { Idle, Checking, Downloading, Ready };

    explicit UpdateFlow(QObject* parent = nullptr);

    // 启动后的静默检查：用户关了开关、或距上次检查不足 24 小时则跳过。
    // 命中新版本只发 newVersionFound（装配层拿去弹气泡），不动界面状态。
    void autoCheck();

    // 手动检查（设置 → 关于 的「检查更新」按钮）。
    void check();

    // 下载发现的版本并解压，完成后弹确认框换装重启；Ready 态再调只重弹确认。
    void downloadAndInstall();

    State state() const { return m_state; }
    bool hasUpdate() const { return !m_available.version.isEmpty(); }
    bool canDownload() const { return hasUpdate() && !m_available.downloadUrl.isEmpty(); }
    const Release& available() const { return m_available; }

    // 当前状态的中文说明，设置页直接显示。
    QString statusText() const;

signals:
    void changed();  // 状态/文案变化（设置页据此刷新按钮与说明）
    void newVersionFound(const QString& version);  // 静默检查命中新版本

public slots:
    // 工作线程任务跑完后经 QMetaObject::invokeMethod 队列到主线程调用。
    void onChecked(const Latest& latest, bool silent);
    void onDownloaded(const QString& sourceDir, const QString& error);

private:
    void confirmInstall();

    State m_state = State::Idle;
    Release m_available;    // 发现的新版本（Idle 且为空 = 无更新）
    QString m_error;        // 最近一次失败原因（配合状态生成文案）
    QString m_zip;          // 安装包落盘路径
    QString m_workDir;      // 解压工作目录（换装脚本收尾清理）
    QString m_sourceDir;    // 解压出的新版本内容根
    qint64 m_received = 0;  // 已下载字节数（定时器轮询 zip 文件大小）
    bool m_checked = false; // 本次运行检查过没有（决定 Idle 态文案）
    QTimer* m_poll = nullptr;
};

}  // namespace update
}  // namespace zpin
