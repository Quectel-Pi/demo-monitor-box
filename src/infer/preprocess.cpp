#include "infer/preprocess.h"

#include <cstdio>
#include <cstring>

#include <rga/im2d.h>
#include <rga/rga.h>

namespace mb {
namespace {

// zoo 的 crop_and_scale_image_c, 只保留 RGB888 (channel=3) 这一路。
// 双线性 + 边缘钳位; 边界处理故意和原实现一致 (到最右列取左边像素, 到最下行取上一行).
void crop_scale_rgb888(const uint8_t* src, int src_w, int src_h, const LetterboxRect& crop,
                       uint8_t* dst, int dst_w, const LetterboxRect& box) {
    const float x_ratio = (float)crop.width / (float)box.width;
    const float y_ratio = (float)crop.height / (float)box.height;
    const int ch = 3;

    for (int dy = box.y; dy < box.y + box.height; ++dy) {
        for (int dx = box.x; dx < box.x + box.width; ++dx) {
            const int ox = dx - box.x;
            const int oy = dy - box.y;

            const int sx = (int)(ox * x_ratio) + crop.x;
            const int sy = (int)(oy * y_ratio) + crop.y;

            const float x_diff = (ox * x_ratio) - (sx - crop.x);
            const float y_diff = (oy * y_ratio) - (sy - crop.y);

            const int index1 = sy * src_w * ch + sx * ch;
            int index2 = index1 + src_w * ch;
            if (sy == src_h - 1) index2 = index1 - src_w * ch;
            int index3 = index1 + ch;
            int index4 = index2 + ch;
            if (sx == src_w - 1) {
                index3 = index1 - ch;
                index4 = index2 - ch;
            }

            const float w00 = (1.0f - x_diff) * (1.0f - y_diff);
            const float w10 = x_diff * (1.0f - y_diff);
            const float w01 = (1.0f - x_diff) * y_diff;
            const float w11 = x_diff * y_diff;

            for (int c = 0; c < ch; ++c) {
                const float v = src[index1 + c] * w00 + src[index3 + c] * w10 + src[index2 + c] * w01 +
                                src[index4 + c] * w11;
                dst[((size_t)dy * dst_w + dx) * ch + c] = (uint8_t)v;
            }
        }
    }
}

// RGA 那条路。返回 false 表示硬件没吃下这张图, 调用方回退 CPU。
bool letterbox_rga(const ImageRGB& src, ImageRGB* dst, const LetterboxRect& box) {
    rga_buffer_t rga_src =
        wrapbuffer_virtualaddr_t((void*)src.data.data(), src.width, src.height, src.width, src.height,
                                 RK_FORMAT_RGB_888);
    rga_buffer_t rga_dst = wrapbuffer_virtualaddr_t((void*)dst->data.data(), dst->width, dst->height,
                                                   dst->width, dst->height, RK_FORMAT_RGB_888);
    if (rga_src.width <= 0 || rga_dst.width <= 0) return false;

    if (box.width != dst->width || box.height != dst->height) {
        // imfill 的 color 是逐字节复制的 4 字节: RGB 三通道都填 114, alpha 无所谓。
        int imcolor = 0;
        char* p = reinterpret_cast<char*>(&imcolor);
        p[0] = p[1] = p[2] = p[3] = (char)kLetterboxPad;
        im_rect dst_whole = {0, 0, dst->width, dst->height};
        if (imfill_t(rga_dst, dst_whole, imcolor, 1) <= 0) {
            // 填充失败就自己 memset, 否则未初始化区域会进模型。
            memset(dst->data.data(), kLetterboxPad, dst->size_bytes());
        }
    }

    im_rect srect = {0, 0, src.width, src.height};
    im_rect drect = {box.x, box.y, box.width, box.height};
    im_rect prect;
    memset(&prect, 0, sizeof(prect));
    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));

    // 必须用未修饰名的 7 参 improcess。C++ 重载版 (10/11 参) 在把 im_rect 传给 pat 时会
    // 匹配到 rga_buffer_t 版本并报编译错, 排查过一次了。
    const IM_STATUS st = improcess(rga_src, rga_dst, pat, srect, drect, prect, 0);
    if (st <= 0) {
        printf("[preprocess] improcess failed: %d (%s)\n", (int)st, imStrError_t(st));
        return false;
    }
    return true;
}

}  // namespace

LetterboxRect compute_letterbox(int src_w, int src_h, int dst_w, int dst_h, Letterbox* lb) {
    LetterboxRect box;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        if (lb) *lb = Letterbox{};
        return box;
    }

    const float scale_w = (float)dst_w / (float)src_w;
    const float scale_h = (float)dst_h / (float)src_h;

    float scale = scale_h;
    int resize_w = dst_w;
    int resize_h = dst_h;
    if (scale_w < scale_h) {
        scale = scale_w;
        resize_h = (int)(src_h * scale);
    } else {
        resize_w = (int)(src_w * scale);
    }

    // zoo 的 allow_slight_change: 宽 4 对齐、高 2 对齐, 让 RGA 走整块拷贝。
    if (resize_w % 4 != 0) resize_w -= resize_w % 4;
    if (resize_h % 2 != 0) resize_h -= resize_h % 2;
    if (resize_w > dst_w) resize_w = dst_w;
    if (resize_h > dst_h) resize_h = dst_h;
    if (resize_w <= 0) resize_w = dst_w;
    if (resize_h <= 0) resize_h = dst_h;

    box.width = resize_w;
    box.height = resize_h;

    if (scale_w < scale_h) {
        box.y = (dst_h - resize_h) / 2;
        if (box.y % 2 != 0) box.y -= box.y % 2;
        if (box.y < 0) box.y = 0;
        box.x = 0;
    } else {
        box.x = (dst_w - resize_w) / 2;
        if (box.x % 2 != 0) box.x -= box.x % 2;
        if (box.x < 0) box.x = 0;
        box.y = 0;
    }

    if (lb) {
        lb->scale = scale;
        lb->x_pad = box.x;
        lb->y_pad = box.y;
    }
    return box;
}

bool letterbox_rgb(const ImageRGB& src, ImageRGB* dst, Letterbox* lb, const PreprocessOptions& opt,
                   const char** warn) {
    if (warn) *warn = nullptr;
    if (!dst || dst->width <= 0 || dst->height <= 0) return false;
    if (src.width <= 0 || src.height <= 0 || src.data.size() < src.size_bytes()) return false;

    dst->data.resize(dst->size_bytes());

    const LetterboxRect box = compute_letterbox(src.width, src.height, dst->width, dst->height, lb);

    // 完全等比且没有 padding 时不需要重采样, 直接整块拷。
    if (box.width == src.width && box.height == src.height && box.x == 0 && box.y == 0 &&
        src.width == dst->width) {
        memcpy(dst->data.data(), src.data.data(), src.size_bytes());
        return true;
    }

    // zoo 只在宽 16 对齐时走 RGA; 不对齐会触发 RGA 的非对齐分支, 结果不可靠。
    if (opt.use_rga && src.width % 16 == 0 && dst->width % 16 == 0) {
        if (letterbox_rga(src, dst, box)) return true;
        if (warn) *warn = "RGA 失败, 已回退 CPU 双线性";
    } else if (opt.use_rga && warn) {
        *warn = "宽度未 16 对齐, 走 CPU 双线性";
    }

    memset(dst->data.data(), kLetterboxPad, dst->size_bytes());
    crop_scale_rgb888(src.data.data(), src.width, src.height,
                      LetterboxRect{0, 0, src.width, src.height}, dst->data.data(), dst->width,
                      box);
    return true;
}

}  // namespace mb
