#ifndef MONITOR_BOX_PREPROCESS_H_
#define MONITOR_BOX_PREPROCESS_H_

#include <cstdint>

#include "infer/types.h"

namespace mb {

// letterbox 填充色。YOLO 训练时用的就是 114, 换成别的颜色会掉点。
constexpr uint8_t kLetterboxPad = 114;

// letterbox 的几何参数: 原图按 scale 缩放后放到 dst 画布的这个矩形里, 其余填 kLetterboxPad。
struct LetterboxRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// 复刻 rknn_model_zoo utils/image_utils.c 的 convert_image_with_letterbox 里那段
// 「allow_slight_change」对齐逻辑 (宽 4 对齐 / 高 2 对齐, 偏移再偶数对齐)。
// 后处理反算框时必须用这里算出来的同样参数, 否则框会系统性偏移一两个像素。
LetterboxRect compute_letterbox(int src_w, int src_h, int dst_w, int dst_h, Letterbox* lb);

struct PreprocessOptions {
    // 允许走 RGA 硬件缩放。宽度没对齐 16 或 RGA 不可用时会自动回退到 CPU 双线性。
    bool use_rga = true;
};

// 把 src 缩放 + 居中填充到 dst (dst.width x dst.height 必须已是模型输入尺寸)。
// 返回实际使用的 letterbox 参数, 供后处理反算。
bool letterbox_rgb(const ImageRGB& src, ImageRGB* dst, Letterbox* lb, const PreprocessOptions& opt,
                   const char** warn);

}  // namespace mb

#endif  // MONITOR_BOX_PREPROCESS_H_
