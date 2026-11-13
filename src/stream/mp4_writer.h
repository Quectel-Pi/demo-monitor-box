#ifndef MONITOR_BOX_STREAM_MP4_WRITER_H_
#define MONITOR_BOX_STREAM_MP4_WRITER_H_

#include <string>
#include <vector>

#include "video/h264_encoder.h"

namespace mb {

// 把一串 Annex-B 访问单元封成一个普通 (非分片) MP4, moov 在前 (faststart),
// 浏览器 <video> 和 APP 的 GStreamer 都能直接播放 / 拖动。
//
// 要求: 第一个包是 IDR 且带 SPS/PPS (编码器配置保证了这一点); 之前的非 IDR 包会被丢掉。
// 编码器不出 B 帧, 所以不写 ctts。
bool write_mp4(const std::string& path, const std::vector<const EncodedPacket*>& packets, int width,
               int height, std::string* err);

}  // namespace mb

#endif  // MONITOR_BOX_STREAM_MP4_WRITER_H_
