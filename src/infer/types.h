#ifndef MONITOR_BOX_INFER_TYPES_H_
#define MONITOR_BOX_INFER_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mb {

// 连续 RGB888 图像 (stride 恒等于 width*3, 不做行对齐)。
struct ImageRGB {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;

    uint8_t* row(int y) { return data.data() + (size_t)y * width * 3; }
    const uint8_t* row(int y) const { return data.data() + (size_t)y * width * 3; }
    size_t size_bytes() const { return (size_t)width * height * 3; }
};

// letterbox 反变换参数。字段语义与 rknn_model_zoo utils/image_utils.h 的 letterbox_t 一致:
//   x_pad / y_pad 是缩放后图像在模型输入画布上的左上角偏移, scale 是统一缩放系数。
struct Letterbox {
    int x_pad = 0;
    int y_pad = 0;
    float scale = 1.0f;
};

// 一个检测结果, 坐标已还原到原始图像像素空间。
struct Detection {
    int cls_id = 0;
    float score = 0.0f;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

}  // namespace mb

#endif  // MONITOR_BOX_INFER_TYPES_H_
