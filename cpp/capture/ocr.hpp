// 文字识别的后台执行壳 —— 本地 PP-OCR（Rust 核心 + ONNX Runtime）一次几十到
// 几百毫秒，必须离开 UI 线程；回调一律回主线程。
// 结果落地姿势（复制剪贴板 + 抛气泡）也收在这里，
// 免得选区与贴图两条路径各写一份文案。
#pragma once

#include <QImage>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

#include "rcore.hpp"

namespace zpin::ocr {

// 异步识别；onDone 在主线程被调，nullopt = 引擎不可用或识别失败。
void recognizeAsync(const QImage& img, std::function<void(std::optional<QString>)> onDone);

// 异步识别并带每行文本框（图像局部像素坐标）；一键脱敏用。
void recognizeBoxesAsync(const QImage& img,
                         std::function<void(std::optional<QVector<rcore::OcrBox>>)> onDone);

// 异步识别表格；空 QVector = 图里没有表格（不是失败）。
void recognizeTablesAsync(
    const QImage& img, std::function<void(std::optional<QVector<rcore::OcrTable>>)> onDone);

// 识别结果的统一处理：复制进剪贴板并经 notify(标题, 正文) 抛提示。
// 返回**是否真的复制了内容**——false 表示这次没结果（识别失败 / 没识别到），
// 调用方据此决定选区会话留不留：没拿到东西就把界面收掉，用户两头落空。
bool publishText(const std::optional<QString>& text,
                 const std::function<void(QString, QString)>& notify);

// 表格结果同上，但剪贴板同时放 text/html 与 text/plain：
// 粘进 Excel/WPS 得到带合并单元格的表，粘进只认纯文本的目标也还是分行分列的表。
bool publishTables(const std::optional<QVector<rcore::OcrTable>>& tables,
                   const std::function<void(QString, QString)>& notify);

// 这行文本像不像需要打码的敏感信息（手机号 / 邮箱 / 身份证）。判据全项目只有这一份：
// 选区与贴图两条「一键脱敏」路径共用，免得一边改了正则另一边漏打。
bool looksSensitiveText(const QString& text);

}  // namespace zpin::ocr
