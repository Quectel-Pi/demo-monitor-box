#ifndef MONITOR_BOX_STREAM_RTP_SENDER_H_
#define MONITOR_BOX_STREAM_RTP_SENDER_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace mb {

// H.264 over RTP/UDP (RFC 6184, packetization-mode=1: 单 NAL + FU-A)。
//
// 接收端就是 APP 的 udp 源:
//   udpsrc port=15004 caps=application/x-rtp,media=video,encoding-name=H264,payload=96
//   ! rtph264depay ! h264parse ! mppvideodec
// caps 里没有 sprop-parameter-sets, 所以 SPS/PPS 必须带内发送 —— 编码器配成了
// 每个 IDR 前都带头, 这里原样转发即可。
class RtpSender {
public:
    RtpSender() = default;
    ~RtpSender();

    RtpSender(const RtpSender&) = delete;
    RtpSender& operator=(const RtpSender&) = delete;

    bool open(const std::string& host, int port, int mtu, std::string* err);
    void close();
    bool is_open() const { return fd_ >= 0; }

    // 发送一个 Annex-B 访问单元; ts_ms 换算到 90kHz。
    void send_access_unit(const uint8_t* data, size_t size, uint64_t ts_ms);

    uint64_t packets_sent() const { return packets_; }
    uint64_t send_errors() const { return errors_; }

private:
    void send_packet(const uint8_t* payload, size_t n, bool marker, uint32_t ts90);

    int fd_ = -1;
    int mtu_ = 1400;
    uint8_t dst_[32] = {0};  // sockaddr_in
    unsigned dst_len_ = 0;
    uint16_t seq_ = 0;
    uint32_t ssrc_ = 0;
    uint64_t packets_ = 0;
    uint64_t errors_ = 0;
};

}  // namespace mb

#endif  // MONITOR_BOX_STREAM_RTP_SENDER_H_
