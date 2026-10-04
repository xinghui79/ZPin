#include "smart_erase.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

#include "logging.hpp"

namespace zpin::smarterase {
namespace {

// 工作缓冲上限（像素数）：洞再大也不许把内存吃穿。超过就直接放弃修复，
// 调用方拿空图回去当「没擦成」处理。
constexpr qint64 kMaximumWorkingPixels = 16 * 1024 * 1024;

// 取消检查：cancelled 由 SmartErase 形状持有（shapes.hpp 的 cancel 字段），
// 形状析构即置位。抛出的异常被 erase() 末尾的 catch 统一转成空图。
void checkCancelled(const std::atomic_bool& cancelled) {
    if (cancelled.load(std::memory_order_relaxed)) {
        throw std::runtime_error("cancelled");
    }
}

// ---- 快路径一：平面拟合 ----
// 洞周围的背景如果近似一块渐变平面（纯色墙、天空、渐变底），用洞外沿一圈
// 观测做鲁棒线性拟合（迭代加权，抗笔迹边缘的杂色像素），拟不上（残差大、
// 内点少）就返回空图交给下一条路。拟合只用洞外像素，洞内永远不参与。
cv::Mat3f planeFill(const cv::Mat3f& source, const cv::Mat1b& hole, const cv::Mat1b& known,
                    const std::atomic_bool& cancelled) {
    const auto bounds = cv::boundingRect(hole);
    cv::Mat1b ring;
    cv::dilate(hole, ring, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(9, 9)));
    cv::bitwise_and(ring, known, ring);
    std::vector<cv::Point> boundary;
    cv::findNonZero(ring, boundary);
    if (boundary.size() < 12)
        return {};
    const auto basis = [&](cv::Point p) {
        return cv::Vec3d(1, static_cast<double>(p.x - bounds.x) / bounds.width,
                         static_cast<double>(p.y - bounds.y) / bounds.height);
    };
    const std::size_t stride = std::max<std::size_t>(1, boundary.size() / 4096);
    cv::Matx33d coefficients = cv::Matx33d::zeros();
    for (int iteration = 0; iteration < 5; ++iteration) {
        checkCancelled(cancelled);
        cv::Matx33d normal = cv::Matx33d::zeros(), rhs = cv::Matx33d::zeros();
        for (std::size_t i = 0; i < boundary.size(); i += stride) {
            const auto p = boundary[i];
            const auto b = basis(p);
            const cv::Vec3d value(source(p));
            const double residual = cv::norm(coefficients.t() * b - value);
            const double weight =
                iteration == 0 ? 1 : std::min(1.0, 0.008 / std::max(1e-8, residual));
            normal += weight * b * b.t();
            rhs += weight * b * value.t();
        }
        if (!cv::solve(normal, rhs, coefficients, cv::DECOMP_CHOLESKY))
            return {};
    }
    std::size_t inliers = 0, count = 0;
    double squaredError = 0;
    for (std::size_t i = 0; i < boundary.size(); i += stride) {
        const auto p = boundary[i];
        const double residual = cv::norm(coefficients.t() * basis(p) - cv::Vec3d(source(p)));
        ++count;
        if (residual < 4.0 / 255) {
            ++inliers;
            squaredError += residual * residual;
        }
    }
    if (inliers * 100 < count * 97 ||
        squaredError > static_cast<double>(inliers) * 6.0 / (255 * 255))
        return {};
    cv::Mat3f result = source.clone();
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
        checkCancelled(cancelled);
        for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
            if (hole(y, x))
                result(y, x) = cv::Vec3f(coefficients.t() * basis({x, y}));
        }
    }
    return result;
}

// ---- 采样源区域 ----
// 块匹配的「供体」只能取洞外的真实像素。先在洞附近找，找不到（笔画贴着
// 画面边缘之类）再成倍扩大搜索半径，直到凑够可用块或到顶。
cv::Mat1b donorRegion(const cv::Mat1b& hole, const cv::Mat1b& known,
                      const std::atomic_bool& cancelled) {
    cv::Mat1f distance;
    cv::distanceTransform(~hole, distance, cv::DIST_L2, 5);
    const auto bounds = cv::boundingRect(hole);
    int reach = std::clamp(std::min(bounds.width, bounds.height) / 2, 16, 96);
    cv::Mat1b domain;
    for (;;) {
        checkCancelled(cancelled);
        cv::compare(distance, reach, domain, cv::CMP_LE);
        cv::bitwise_and(domain, known, domain);
        cv::Mat1b patches;
        cv::erode(domain, patches, cv::getStructuringElement(cv::MORPH_RECT, {7, 7}), {-1, -1}, 1,
                  cv::BORDER_CONSTANT, cv::Scalar(0));
        if (cv::countNonZero(patches) >= 64 || reach >= 512)
            return domain;
        reach = std::min(512, reach * 2);
    }
}

// ---- 快路径二：周期纹理平铺 ----
// 背景是规则纹理（砖墙、织物、栅栏）时，找到一个平移周期并验证它对整个
// 采样区域都成立（只对上一行成立不够），成立就直接搬像素，毫秒级出结果。
cv::Mat3f periodicTileFill(const cv::Mat3f& source, const cv::Mat1b& hole,
                           const cv::Mat1b& domain, const std::atomic_bool& cancelled) {
    std::vector<cv::Point> points;
    cv::findNonZero(domain, points);
    for (const auto direction : {cv::Point(1, 0), cv::Point(0, 1)}) {
        for (int period = 2; period <= 64; ++period) {
            checkCancelled(cancelled);
            const auto offset = direction * period;
            int count = 0;
            double error = 0;
            for (std::size_t i = 0; i < points.size(); ++i) {
                if (i % 4096 == 0)
                    checkCancelled(cancelled);
                const auto p = points[i], q = p + offset;
                if (!cv::Rect({}, source.size()).contains(q) || !domain(q))
                    continue;
                const auto delta = source(p) - source(q);
                error += static_cast<double>(delta.dot(delta));
                ++count;
                // 早期否决：误差上限按最终采样总数封顶，超了直接换下一个周期
                if (error > static_cast<double>(points.size()) * 0.5 / (255 * 255))
                    break;
            }
            if (count < 64 || error > count * 0.5 / (255 * 255))
                continue;
            cv::Mat3f output = source.clone();
            bool complete = true;
            for (int y = 0; y < source.rows && complete; ++y) {
                checkCancelled(cancelled);
                for (int x = 0; x < source.cols; ++x) {
                    if (!hole(y, x))
                        continue;
                    bool found = false;
                    for (int d = 1; d * period < std::max(source.rows, source.cols) && !found;
                         ++d) {
                        for (const int sign : {-1, 1}) {
                            const cv::Point donor = cv::Point(x, y) + offset * (d * sign);
                            if (cv::Rect({}, source.size()).contains(donor) && domain(donor)) {
                                output(y, x) = source(donor);
                                found = true;
                                break;
                            }
                        }
                    }
                    if (!found) {
                        complete = false;
                        break;
                    }
                }
            }
            if (complete)
                return output;
        }
    }
    return {};
}

// ---- 慢路径的引导场 ----
// 背景均值与纹理幅度：只在已知像素上加权（高斯），洞内的估计值由周围
// 观测外推而来，作为块匹配的颜色/纹理「锚」。
cv::Mat3f backgroundMean(const cv::Mat3f& source, const cv::Mat1b& known, int radius,
                         const std::atomic_bool& cancelled, cv::Mat1f& weights) {
    known.convertTo(weights, CV_32F, 1.0 / 255);
    cv::Mat3f weighted(source.size(), cv::Vec3f(0, 0, 0));
    source.copyTo(weighted, known);
    const cv::Size kernel(2 * radius + 1, 2 * radius + 1);
    cv::GaussianBlur(weighted, weighted, kernel, radius / 2.0);
    cv::GaussianBlur(weights, weights, kernel, radius / 2.0);
    for (int y = 0; y < source.rows; ++y) {
        checkCancelled(cancelled);
        for (int x = 0; x < source.cols; ++x)
            weighted(y, x) /= std::max(weights(y, x), 1e-6F);
    }
    return weighted;
}

cv::Mat1f backgroundTexture(const cv::Mat3f& source, const cv::Mat3f& mean,
                            const cv::Mat1b& known, int radius,
                            const std::atomic_bool& cancelled, const cv::Mat1f& weights) {
    cv::Mat1f squared(source.size(), 0.0F);
    for (int y = 0; y < source.rows; ++y) {
        checkCancelled(cancelled);
        for (int x = 0; x < source.cols; ++x)
            if (known(y, x))
                squared(y, x) = source(y, x).dot(source(y, x));
    }
    const cv::Size kernel(2 * radius + 1, 2 * radius + 1);
    cv::GaussianBlur(squared, squared, kernel, radius / 2.0);
    for (int y = 0; y < source.rows; ++y) {
        checkCancelled(cancelled);
        for (int x = 0; x < source.cols; ++x)
            squared(y, x) = std::sqrt(std::max(
                0.0F, squared(y, x) / std::max(weights(y, x), 1e-6F) - mean(y, x).dot(mean(y, x))));
    }
    return squared;
}

struct SpanGuide {
    cv::Mat3f color;
    cv::Mat1f variation;
};

// 跨洞引导：沿四个方向把洞两侧的观测「拉通」——两侧端点颜色一致就线性过渡
//（保住横穿的条带/边缘），不一致就取近端。谁先给出更低成本的搭桥谁留下。
SpanGuide spanGuide(const cv::Mat3f& observed, const cv::Mat1f& variation,
                    const cv::Mat1b& hole, const cv::Mat1b& known,
                    const std::atomic_bool& cancelled) {
    cv::Mat3f guide = observed.clone();
    cv::Mat1f texture = variation.clone();
    cv::Mat1f costs(observed.size(), std::numeric_limits<float>::max());
    const cv::Rect image({}, observed.size());
    for (const auto step : {cv::Point(1, 0), cv::Point(0, 1), cv::Point(1, 1), cv::Point(-1, 1)}) {
        for (int y = 0; y < observed.rows; ++y) {
            checkCancelled(cancelled);
            for (int x = 0; x < observed.cols; ++x) {
                if (image.contains(cv::Point(x, y) - step))
                    continue;
                cv::Point before(-1, -1);
                std::vector<cv::Point> pending;
                const auto flush = [&](cv::Point after) {
                    if (pending.empty())
                        return;
                    if (before.x < 0 && after.x < 0)
                        return;
                    const auto a = before.x >= 0 ? before : after;
                    const auto b = after.x >= 0 ? after : before;
                    const auto delta = observed(a) - observed(b);
                    const float difference = delta.dot(delta);
                    const float span = static_cast<float>(cv::norm(a - b));
                    const float score =
                        difference + 0.02F * span + (before.x < 0 || after.x < 0 ? 1000.0F : 0.0F);
                    for (const auto p : pending) {
                        if (score >= costs(p))
                            continue;
                        const float t = span > 0 ? static_cast<float>(cv::norm(p - a)) / span : 0;
                        guide(p) = difference < 900 ? observed(a) * (1 - t) + observed(b) * t
                                                    : observed(t < 0.5F ? a : b);
                        texture(p) = variation(a) * (1 - t) + variation(b) * t;
                        costs(p) = score;
                    }
                };
                for (cv::Point p(x, y); image.contains(p); p += step) {
                    if (known(p)) {
                        flush(p);
                        pending.clear();
                        before = p;
                    } else if (hole(p)) {
                        pending.push_back(p);
                    } else {
                        flush({-1, -1});
                        pending.clear();
                        before = {-1, -1};
                    }
                }
                flush({-1, -1});
            }
        }
    }
    return {guide, texture};
}

struct LevelResult {
    cv::Mat3f image;
    cv::Mat_<cv::Vec2i> matches;
};

struct MaskSpan {
    int y;
    int begin;
    int end;
    std::size_t offset;
};
struct PatchCoverage {
    unsigned char known = 0;
    unsigned char missing = 0;
    bool interior = false;
};
struct TargetPatch {
    int x;
    int y;
    cv::Vec3f color;
    float variation;
    float structureWeight;
    float normalization;
    bool interior;
};

// ---- 慢路径核心：单层金字塔的匹配-投票 ----
// 经典 PatchMatch 三件套的子集：传播（行扫描方向交替 + 邻居搬移）、随机搜索
//（半径逐轮减半）、重用惩罚（同一供体块被抄太多次就抬价）。投票阶段把所有
// 指到自家邻域的匹配加权平均回去，天然带抗锯齿。洞外像素永不改写。
LevelResult fillOneLevel(const cv::Mat3f& source, const cv::Mat1b& hole, const cv::Mat1b& coverage,
                         const cv::Mat1b& domain, const SpanGuide& background,
                         const LevelResult& previous, const std::atomic_bool& cancelled,
                         int passes) {
    const auto& guide = background.color;
    const auto& variation = background.variation;
    cv::Mat1b donors;
    std::vector<cv::Point> candidates;
    int radius = 3;
    for (; radius >= 0; --radius) {
        cv::erode(domain, donors,
                  cv::getStructuringElement(cv::MORPH_RECT, {2 * radius + 1, 2 * radius + 1}),
                  {-1, -1}, 1, cv::BORDER_CONSTANT, cv::Scalar(0));
        cv::findNonZero(donors, candidates);
        if (!candidates.empty())
            break;
    }
    if (candidates.empty())
        throw std::runtime_error("no source patches");
    std::vector<MaskSpan> spans;
    std::vector<PatchCoverage> patches;
    for (int y = 0; y < hole.rows; ++y) {
        checkCancelled(cancelled);
        const auto* row = hole.ptr<unsigned char>(y);
        for (int x = 0; x < hole.cols;) {
            if (!row[x]) {
                ++x;
                continue;
            }
            const int begin = x;
            const auto offset = patches.size();
            for (; x < hole.cols && row[x]; ++x) {
                PatchCoverage patch;
                for (int dy = -radius; dy <= radius; ++dy) {
                    const int ty = y + dy;
                    if (ty < 0 || ty >= hole.rows)
                        continue;
                    const auto* covered = coverage.ptr<unsigned char>(ty);
                    const auto* masked = hole.ptr<unsigned char>(ty);
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int tx = x + dx;
                        if (tx < 0 || tx >= hole.cols || !covered[tx])
                            continue;
                        if (masked[tx])
                            ++patch.missing;
                        else
                            ++patch.known;
                    }
                }
                patch.interior = patch.known + patch.missing == (2 * radius + 1) * (2 * radius + 1);
                patches.push_back(patch);
            }
            spans.push_back({y, begin, x, offset});
        }
    }
    cv::Mat3f output = source.clone();
    if (!previous.image.empty()) {
        cv::Mat3f enlarged;
        cv::resize(previous.image, enlarged, source.size(), 0, 0, cv::INTER_LINEAR);
        enlarged.copyTo(output, hole);
    }
    cv::Mat1f distance;
    cv::Mat1i labels;
    cv::distanceTransform(~donors, distance, labels, cv::DIST_L2, 5, cv::DIST_LABEL_PIXEL);
    std::vector<cv::Point> nearest(candidates.size() + 1);
    for (const auto p : candidates)
        nearest.at(static_cast<std::size_t>(labels(p))) = p;
    cv::Mat_<cv::Vec2i> matches(source.size(), cv::Vec2i(-1, -1));
    cv::Mat1f costs(source.size(), std::numeric_limits<float>::max());
    // 每轮完整的使用统计在下轮开头折算成惩罚，压住「一块好砖抄全墙」
    cv::Mat1f reusePenalty(source.size(), 0.0F);
    const float expectedUsage =
        std::max(1.0F, static_cast<float>(patches.size()) / static_cast<float>(candidates.size()));
    std::mt19937 random(0x5A50494EU);  // 固定种子：同图同 mask 结果可复现（可测试）
    if (previous.image.empty()) {
        for (const auto& span : spans) {
            checkCancelled(cancelled);
            const int y = span.y;
            for (int x = span.begin; x < span.end; ++x) {
                auto best = nearest.at(static_cast<std::size_t>(labels(y, x)));
                auto delta = guide(y, x) - guide(best);
                float textureDelta = variation(y, x) - variation(best);
                float error = delta.dot(delta) + 4 * textureDelta * textureDelta;
                for (int sample = 0; sample < 32; ++sample) {
                    const auto p = candidates[random() % candidates.size()];
                    delta = guide(y, x) - guide(p);
                    textureDelta = variation(y, x) - variation(p);
                    const float cost = delta.dot(delta) + 4 * textureDelta * textureDelta;
                    if (cost < error) {
                        best = p;
                        error = cost;
                    }
                }
                output(y, x) = source(best);
                matches(y, x) = {best.x, best.y};
            }
        }
    }
    float synthesizedWeight = 0.05F;
    const float scale = static_cast<float>(std::max(source.cols, source.rows));
    const auto targetPatch = [&](const MaskSpan& span, int x) {
        const auto& patch = patches[span.offset + static_cast<std::size_t>(x - span.begin)];
        const float texture = variation(span.y, x);
        return TargetPatch{x,
                           span.y,
                           guide(span.y, x),
                           texture,
                           8.0F / (4.0F + texture),
                           std::max(1.0F, patch.known + patch.missing * synthesizedWeight),
                           patch.interior};
    };
    const auto tryCandidate = [&](const TargetPatch& target, int sx, int sy) {
        if (sx < 0 || sy < 0 || sx >= source.cols || sy >= source.rows || !donors(sy, sx))
            return;
        const auto structure = target.color - guide(sy, sx);
        const float textureDelta = target.variation - variation(sy, sx);
        const float dx = static_cast<float>(sx - target.x), dy = static_cast<float>(sy - target.y);
        const float fixed = target.structureWeight * structure.dot(structure) +
                            4 * textureDelta * textureDelta + reusePenalty(sy, sx) +
                            8 * (dx * dx + dy * dy) / (scale * scale);
        const float best = costs(target.y, target.x);
        // 省掉的平方差都非负：用完整分母外加一点点余量做剪枝，舍入不会
        // 错杀有竞争力的块
        const float limit = best + 1e-5F * std::max(1.0F, best);
        if (fixed > limit)
            return;
        float sum = 0;
        const auto accumulate = [&](auto interior) {
            for (int py = -radius; py <= radius; ++py) {
                const int ty = target.y + py;
                if constexpr (!decltype(interior)::value) {
                    if (ty < 0 || ty >= source.rows)
                        continue;
                }
                const auto* targetRow = output.ptr<cv::Vec3f>(ty);
                const auto* sourceRow = source.ptr<cv::Vec3f>(sy + py);
                const auto* holeRow = hole.ptr<unsigned char>(ty);
                const auto* coverageRow = coverage.ptr<unsigned char>(ty);
                for (int px = -radius; px <= radius; ++px) {
                    const int tx = target.x + px;
                    if constexpr (!decltype(interior)::value) {
                        if (tx < 0 || tx >= source.cols || !coverageRow[tx])
                            continue;
                    }
                    const auto delta = targetRow[tx] - sourceRow[sx + px];
                    sum += delta.dot(delta) * (holeRow[tx] ? synthesizedWeight : 1.0F);
                }
                if (sum / target.normalization + fixed > limit)
                    return false;
            }
            return true;
        };
        if (!(target.interior ? accumulate(std::true_type{}) : accumulate(std::false_type{})))
            return;
        const float cost = sum / target.normalization + fixed;
        if (cost < best) {
            costs(target.y, target.x) = cost;
            matches(target.y, target.x) = {sx, sy};
        }
    };
    if (!previous.matches.empty()) {
        for (const auto& span : spans) {
            checkCancelled(cancelled);
            const int y = span.y;
            for (int x = span.begin; x < span.end; ++x) {
                const int px =
                    static_cast<int>(static_cast<qint64>(x) * previous.matches.cols / source.cols);
                const int py =
                    static_cast<int>(static_cast<qint64>(y) * previous.matches.rows / source.rows);
                const auto p = previous.matches(py, px);
                if (p[0] >= 0) {
                    const int sx = static_cast<int>(static_cast<qint64>(p[0]) * source.cols /
                                                    previous.matches.cols);
                    const int sy = static_cast<int>(static_cast<qint64>(p[1]) * source.rows /
                                                    previous.matches.rows);
                    tryCandidate(targetPatch(span, x), sx, sy);
                }
            }
        }
    }
    cv::Mat3f next = source.clone();
    for (int iteration = 0; iteration < passes; ++iteration) {
        synthesizedWeight = 0.15F + 0.20F * static_cast<float>(iteration) / (passes - 1);
        for (const auto p : candidates)
            reusePenalty(p) = 0;
        for (const auto& span : spans) {
            checkCancelled(cancelled);
            for (int x = span.begin; x < span.end; ++x) {
                const auto p = matches(span.y, x);
                if (p[0] >= 0)
                    reusePenalty(p[1], p[0]) += 1;
            }
        }
        for (const auto p : candidates)
            reusePenalty(p) = 25 * std::log1p(reusePenalty(p) / expectedUsage);
        const int step = iteration % 2 == 0 ? 1 : -1;
        for (std::size_t si = 0; si < spans.size(); ++si) {
            checkCancelled(cancelled);
            const auto& span = spans[step > 0 ? si : spans.size() - 1 - si];
            const int y = span.y;
            for (int xi = 0; xi < span.end - span.begin; ++xi) {
                if (xi % 64 == 0)
                    checkCancelled(cancelled);
                const int x = step > 0 ? span.begin + xi : span.end - 1 - xi;
                costs(y, x) = std::numeric_limits<float>::max();
                const auto target = targetPatch(span, x);
                const auto old = matches(y, x);
                tryCandidate(target, old[0], old[1]);
                const auto local = nearest.at(static_cast<std::size_t>(labels(y, x)));
                tryCandidate(target, local.x, local.y);
                const auto seed = candidates[random() % candidates.size()];
                tryCandidate(target, seed.x, seed.y);
                if (x - step >= 0 && x - step < source.cols) {
                    const auto neighbor = matches(y, x - step);
                    if (neighbor[0] >= 0)
                        tryCandidate(target, neighbor[0] + step, neighbor[1]);
                }
                if (y - step >= 0 && y - step < source.rows) {
                    const auto neighbor = matches(y - step, x);
                    if (neighbor[0] >= 0)
                        tryCandidate(target, neighbor[0], neighbor[1] + step);
                }
                for (int window = std::max(source.cols, source.rows); window > 0; window /= 2) {
                    const auto center = matches(y, x);
                    const int width = window * 2 + 1;
                    const int sx = center[0] +
                                   static_cast<int>(random() % static_cast<unsigned>(width)) -
                                   window;
                    const int sy = center[1] +
                                   static_cast<int>(random() % static_cast<unsigned>(width)) -
                                   window;
                    tryCandidate(target, sx, sy);
                }
            }
        }
        const auto vote = [&](const cv::Range& range) {
            for (int si = range.start; si < range.end; ++si) {
                checkCancelled(cancelled);
                const auto& span = spans[static_cast<std::size_t>(si)];
                const int y = span.y;
                for (int x = span.begin; x < span.end; ++x) {
                    cv::Vec3f sum(0, 0, 0);
                    float weight = 0;
                    const auto center = matches(y, x);
                    const auto anchor = source(center[1], center[0]);
                    const float tolerance = std::clamp(variation(y, x) * 0.25F, 1.5F, 10.0F);
                    for (int dy = -radius; dy <= radius; ++dy) {
                        for (int dx = -radius; dx <= radius; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= source.cols || ny >= source.rows ||
                                !hole(ny, nx))
                                continue;
                            const auto donor = matches(ny, nx);
                            if (donor[0] < 0)
                                continue;
                            const auto value = source(donor[1] - dy, donor[0] - dx);
                            const auto difference =
                                guide(donor[1] - dy, donor[0] - dx) - guide(y, x);
                            const auto detailDifference = value - anchor;
                            const float w = 1.0F / ((1.0F + costs(ny, nx)) *
                                                    (1.0F + difference.dot(difference) / 100.0F) *
                                                    (1.0F + detailDifference.dot(detailDifference) /
                                                                (tolerance * tolerance)));
                            sum += value * w;
                            weight += w;
                        }
                    }
                    // 已知像素永远保持原观测，两个缓冲都一样
                    next(y, x) = weight > 0 ? sum / weight : output(y, x);
                }
            }
        };
        const cv::Range range(0, static_cast<int>(spans.size()));
        if (patches.size() >= 32768)
            cv::parallel_for_(range, vote, 2);
        else
            vote(range);
        std::swap(output, next);
    }
    return {output, matches};
}

}  // namespace

QImage erase(const QImage& baseImg, const QPainterPath& maskPath,
             const std::atomic_bool& cancelled, const Options& options) {
    try {
        // 轮数钳在 [2,5]：fillOneLevel 里 passes-1 是除数，1 会除零
        const Options opts{std::clamp(options.coarsePasses, 2, 5),
                           std::clamp(options.intermediatePasses, 2, 5),
                           std::clamp(options.finePasses, 2, 5)};
        const QImage base = baseImg.convertToFormat(QImage::Format_ARGB32);
        if (base.isNull() || base.width() <= 0 || base.height() <= 0 || maskPath.isEmpty()) {
            log::warn("smarterase",
                      QStringLiteral("输入无效：底图 %1×%2（null=%3），mask 空=%4")
                          .arg(baseImg.width())
                          .arg(baseImg.height())
                          .arg(baseImg.isNull())
                          .arg(maskPath.isEmpty()));
            return {};
        }
        const cv::Rect fullRect(0, 0, base.width(), base.height());
        // 洞的外接矩形（裁进图内），四周再留一段供体上下文：留太少，慢路径
        // 的引导核和块核会拿不到足够的真实像素
        QRectF targetF = maskPath.boundingRect().intersected(QRectF(fullRect.x, fullRect.y,
                                                                    fullRect.width, fullRect.height));
        if (targetF.isEmpty())
            return {};
        const QRect target = targetF.toAlignedRect();
        const int padding = std::min(512, std::max(64, std::max(target.width(), target.height())));
        const QRect roi = target.adjusted(-padding, -padding, padding, padding)
                              .intersected(QRect(QPoint(0, 0), base.size()));
        const cv::Rect roiCv(roi.x(), roi.y(), roi.width(), roi.height());
        if (roiCv.width <= 0 || roiCv.height <= 0 ||
            static_cast<qint64>(roiCv.width) * roiCv.height > kMaximumWorkingPixels) {
            log::warn("smarterase", QStringLiteral("工作区不可用：%1×%2（上限 %3 MP）")
                                        .arg(roiCv.width)
                                        .arg(roiCv.height)
                                        .arg(kMaximumWorkingPixels / 1000000));
            return {};
        }
        checkCancelled(cancelled);

        // 栅格化 mask（ROI 内）：抗锯齿填充后 >0 即在洞内
        QImage maskImg(roi.width(), roi.height(), QImage::Format_Grayscale8);
        if (maskImg.isNull())
            return {};
        maskImg.fill(0);
        {
            QPainter p(&maskImg);
            p.setRenderHint(QPainter::Antialiasing);
            p.translate(-roi.x(), -roi.y());
            p.fillPath(maskPath, Qt::white);
        }

        QImage originalRoi = base.copy(roi);
        cv::Mat1b hole(roi.height(), roi.width()), coverage(roi.height(), roi.width());
        cv::Mat3f rgb(roi.height(), roi.width());
        for (int y = 0; y < roi.height(); ++y) {
            checkCancelled(cancelled);
            const auto* row = reinterpret_cast<const QRgb*>(originalRoi.constScanLine(y));
            const auto* maskRow = maskImg.constScanLine(y);
            for (int x = 0; x < roi.width(); ++x) {
                // 底图是不透明的（截图没有透明通道语义），coverage 恒全 1
                coverage(y, x) = 255;
                hole(y, x) = maskRow[x] > 0 ? 255 : 0;
                rgb(y, x) = cv::Vec3f(static_cast<float>(qRed(row[x])) / 255.0F,
                                      static_cast<float>(qGreen(row[x])) / 255.0F,
                                      static_cast<float>(qBlue(row[x])) / 255.0F);
            }
        }
        if (cv::countNonZero(hole) == 0) {
            log::warn("smarterase", QStringLiteral("mask 栅格化为空（框 %1×%2）")
                                        .arg(roi.width())
                                        .arg(roi.height()));
            return {};
        }
        cv::Mat1b known;
        cv::bitwise_and(coverage, ~hole, known);
        if (cv::countNonZero(known) == 0) {
            log::warn("smarterase", QStringLiteral("洞外没有可用像素（框几乎盖满全图）"));
            return {};
        }

        // 快路径先行：平面拟合 → 周期纹理；都不行才走多尺度块匹配。
        // filledRoi 恒为 ROI 尺寸（慢路径的结果是 workRect 子块几何，
        // 拷回 ROI 坐标系），写回段就不用分两条路算偏移
        cv::Mat3f surface = planeFill(rgb, hole, known, cancelled);
        const auto domain = surface.empty() ? donorRegion(hole, known, cancelled) : cv::Mat1b();
        if (surface.empty())
            surface = periodicTileFill(rgb, hole, domain, cancelled);
        cv::Mat3f filledRoi;
        if (!surface.empty()) {
            filledRoi = surface;
        } else {
            const auto bounds = cv::boundingRect(hole);
            const int guideRadius = std::clamp(std::min(bounds.width, bounds.height) / 12, 4, 16);
            // 把工作区收紧到「洞 + 供体」的活跃范围，引导核和块核的观测都留在
            // 界内。钳制必须用 ROI 的局部尺寸：domain 最远能伸出洞 512px，超出
            // ROI 预留边（≥64px）是常态——钳到整图尺寸的话工作矩形会越过
            // hole() 的边界，取子矩阵直接断言炸掉（纯白背景走快路径到不了
            // 这里，所以当年只表现为「复杂背景必失败」）
            const auto active = cv::boundingRect(hole | domain);
            const int margin = guideRadius + 3;
            const cv::Rect workRect =
                cv::Rect(active.x - margin, active.y - margin, active.width + 2 * margin,
                         active.height + 2 * margin) &
                cv::Rect(0, 0, roi.width(), roi.height());
            const auto patchHole = hole(workRect);
            const auto patchCoverage = coverage(workRect);
            const auto patchKnown = known(workRect);
            cv::Mat3f lab;
            cv::cvtColor(rgb(workRect), lab, cv::COLOR_RGB2Lab);
            cv::Mat1f weights;
            const auto mean = backgroundMean(lab, patchKnown, guideRadius, cancelled, weights);
            const auto variation =
                backgroundTexture(lab, mean, patchKnown, guideRadius, cancelled, weights);
            weights.release();
            const auto guide = spanGuide(mean, variation, patchHole, patchKnown, cancelled);
            std::vector<cv::Mat3f> images{lab};
            std::vector<SpanGuide> guides{guide};
            std::vector<cv::Mat1b> holes{patchHole}, coverages{patchCoverage};
            std::vector<cv::Mat1b> domains{domain(workRect)};
            // 金字塔下降：粗层先定大结构，细层只精修边缘。层太多对齐误差也
            // 会往下传，所以压到 64px 以下就停
            while (std::max(images.back().cols, images.back().rows) > 64 &&
                   std::min(images.back().cols, images.back().rows) > 8) {
                checkCancelled(cancelled);
                cv::Mat3f reduced;
                cv::pyrDown(images.back(), reduced);
                cv::Mat1b reducedHole, reducedCoverage, reducedDomain;
                cv::Mat1b expandedHole;
                cv::dilate(holes.back(), expandedHole,
                           cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));
                cv::resize(expandedHole, reducedHole, reduced.size(), 0, 0, cv::INTER_AREA);
                cv::threshold(reducedHole, reducedHole, 0, 255, cv::THRESH_BINARY);
                cv::Mat1b interiorCoverage, interiorDomain;
                const auto kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5));
                cv::erode(coverages.back(), interiorCoverage, kernel, {-1, -1}, 1,
                          cv::BORDER_CONSTANT, cv::Scalar(0));
                cv::resize(interiorCoverage, reducedCoverage, reduced.size(), 0, 0, cv::INTER_AREA);
                cv::threshold(reducedCoverage, reducedCoverage, 254, 255, cv::THRESH_BINARY);
                cv::erode(domains.back(), interiorDomain, kernel, {-1, -1}, 1, cv::BORDER_CONSTANT,
                          cv::Scalar(0));
                cv::resize(interiorDomain, reducedDomain, reduced.size(), 0, 0, cv::INTER_AREA);
                cv::threshold(reducedDomain, reducedDomain, 254, 255, cv::THRESH_BINARY);
                cv::bitwise_and(reducedDomain, reducedCoverage & ~reducedHole, reducedDomain);
                cv::Mat1b patches;
                cv::erode(reducedDomain, patches, cv::getStructuringElement(cv::MORPH_RECT, {7, 7}),
                          {-1, -1}, 1, cv::BORDER_CONSTANT, cv::Scalar(0));
                if (cv::countNonZero(patches) < 16)
                    break;
                SpanGuide reducedGuide;
                cv::pyrDown(guides.back().color, reducedGuide.color);
                cv::pyrDown(guides.back().variation, reducedGuide.variation);
                images.push_back(reduced);
                guides.push_back(reducedGuide);
                holes.push_back(reducedHole);
                coverages.push_back(reducedCoverage);
                domains.push_back(reducedDomain);
            }
            LevelResult level;
            for (std::size_t i = images.size(); i-- > 0;) {
                const int passes = i == images.size() - 1 ? opts.coarsePasses
                                   : i == 0               ? opts.finePasses
                                                          : opts.intermediatePasses;
                level = fillOneLevel(images[i], holes[i], coverages[i], domains[i], guides[i],
                                     level, cancelled, passes);
            }
            cv::Mat3f lab2rgb;
            cv::cvtColor(level.image, lab2rgb, cv::COLOR_Lab2RGB);
            // 慢路径结果是 workRect 子块几何：先给 filledRoi 分配出 ROI 尺寸，
            // 再 copyTo 写穿视图归位——空 Mat 上取视图会触发尺寸断言
            filledRoi.create(roi.height(), roi.width());
            lab2rgb.copyTo(filledRoi(workRect));
        }

        // 写回：只改洞内像素，其余全部保持原观测
        QImage out = base.copy();
        for (int y = 0; y < roi.height(); ++y) {
            checkCancelled(cancelled);
            auto* row = reinterpret_cast<QRgb*>(out.scanLine(roi.y() + y));
            for (int x = 0; x < roi.width(); ++x) {
                if (!hole(y, x))
                    continue;
                const auto& color = filledRoi(y, x);
                row[roi.x() + x] = qRgba(std::clamp(qRound(color[0] * 255), 0, 255),
                                         std::clamp(qRound(color[1] * 255), 0, 255),
                                         std::clamp(qRound(color[2] * 255), 0, 255),
                                         qAlpha(row[roi.x() + x]));
            }
        }
        return out;
    } catch (const std::exception& e) {
        // 取消与「无法修复」统一表现为空图；失败原因必须落日志，否则用户只
        // 看到红晕不退、完全无从排查
        log::warn("smarterase", QStringLiteral("修复失败：%1").arg(QString::fromUtf8(e.what())));
        return {};
    }
}

}  // namespace zpin::smarterase
