// genico —— 构建期工具：把内嵌 SVG 渲染成 assets/app_icon.ico（exe 资源用）。
// 用法: genico <输出路径>
#include <QGuiApplication>
#include <cstdio>

#include "ui_app_icon.hpp"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        qWarning("用法: genico <输出.ico>");
        return 2;
    }
    if (!zpin::appicon::writeIco(QString::fromLocal8Bit(argv[1]))) {
        qWarning("写入 %s 失败", argv[1]);
        return 1;
    }
    return 0;
}
