// 智能擦除 —— 内容感知填充：把 mask 区域用周围的背景重建掉（水印、敏感文字、
// 乱入的杂物），而不是像马赛克那样盖住。算法是 PatchMatch 风格的修复：
// 先走两条快路径（平面拟合 / 周期纹理平铺），不行再做多尺度金字塔 +
// 块匹配 + 加权投票。整段计算都设计为可在后台线程跑（cancelled 中途取消，
// 抛出由 erase 内部接住转为空图），单次几百毫秒到两秒，取决于洞的大小。
// 依赖 OpenCV（core + imgproc，静态链接）；构建准备见 README。
#pragma once

#include <QImage>
#include <QPainterPath>

#include <atomic>

namespace zpin::smarterase {

struct Options {
    int coarsePasses = 5;        // 金字塔最粗层的匹配-投票轮数
    int intermediatePasses = 3;  // 中间层轮数
    int finePasses = 2;          // 最细层（原图分辨率）轮数
};

// base：底图（物理像素；内部会转成 ARGB32，调用方持有原格式无妨）。
// mask：要擦掉的区域，base 坐标系（引擎用笔画描边路径生成）。
// 返回修复后的完整底图（隐式共享，代价低）；取消或无法修复（例如整个画面
// 都在 mask 里、没有采样来源）返回空图。
QImage erase(const QImage& base, const QPainterPath& mask,
             const std::atomic_bool& cancelled, const Options& options = {});

}  // namespace zpin::smarterase
