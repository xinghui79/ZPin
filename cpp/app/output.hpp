// 保存输出 —— 文件名模板渲染 + 快速保存 / 另存为 + 格式记忆（写盘均在后台线程）。
// 模板令牌：$yyyy-MM-dd_HH-mm-ss$ 等，渲染为当前时间。
// 整屏 PNG 编码约 200ms，放主线程会在保存瞬间卡顿，因此统一走后台线程。
#pragma once

#include <QImage>
#include <QString>
#include <functional>

class QWidget;

namespace zpin::output {

// 把文件名模板里的 $yyyy-MM-dd_HH-mm-ss$ 等令牌渲染为时间字段。
QString renderName(const QString& tmpl);

// 模板对应的「无对话框保存」落盘文件名：渲染令牌后按 saveImageAsync 同一套
// 裁决链补真实后缀（设置页的模板预览用，「默认输出格式」非空时与模板后缀不同）。
QString previewName(const QString& tmpl);

// 保存的落点目录（配置；未配置回退桌面/主目录）。「另存为」对话框的初始位置、
// 工具栏「保存」、「每次截图自动保存一份」补写的那份，全用这一个。
QString defaultDir();

// 按模板把图像异步保存到指定目录（不弹框）。回调在子线程调用：
// 成功传完整路径、失败传空串。
void saveImageAsync(const QImage& img, const QString& ext = {},
                    const QString& directory = {},
                    std::function<void(QString)> onDone = {});

// 弹出「另存为」确定路径后，在工作线程编码写盘。
// 回调在子线程调用：成功传完整路径、失败传空串、用户取消传空 optional。
void saveImageDialogAsync(const QImage& img, QWidget* parent = nullptr,
                          std::function<void(std::optional<QString>)> onDone = {});

// 等待在途写盘全部收尾（进程退出前调）。写盘途中进程被干掉会留下截断的
// PNG/BMP——用户拿到的是「文件存在但打不开」，比报错更难查。写的是本地
// 文件，单张几百毫秒，退出的这点等待是划算的。
void drain();

}  // namespace zpin::output
