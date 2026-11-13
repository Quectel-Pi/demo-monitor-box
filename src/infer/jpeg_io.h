#ifndef MONITOR_BOX_JPEG_IO_H_
#define MONITOR_BOX_JPEG_IO_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "infer/types.h"

namespace mb {

// 用 classic libjpeg API (jpeglib.h) 编解码。
// 注意: sysroot 里没有 turbojpeg.h, 只能用 classic API; 也不要引 OpenCV
// (sysroot 的 lapack/blas 符号链接悬空, 链不上)。

// 从内存解码一张 JPEG, 输出 RGB888。失败返回 false 并填 err。
bool jpeg_decode_mem(const uint8_t* buf, size_t len, ImageRGB* out, std::string* err);

// 实时视频帧解码: 使用快速 DCT 和快速色度上采样, 画质损失很小但能稳定 1080p30。
bool jpeg_decode_mem_fast(const uint8_t* buf, size_t len, ImageRGB* out, std::string* err);

// 读取 JPEG 文件(整个文件读入内存后解码)。
bool jpeg_decode_file(const std::string& path, ImageRGB* out, std::string* err);

// 从内存编码成 JPEG。
bool jpeg_encode_mem(const ImageRGB& img, int quality, std::vector<uint8_t>* out, std::string* err);

// 编码并写文件。
bool jpeg_encode_file(const std::string& path, const ImageRGB& img, int quality, std::string* err);

}  // namespace mb

#endif  // MONITOR_BOX_JPEG_IO_H_
