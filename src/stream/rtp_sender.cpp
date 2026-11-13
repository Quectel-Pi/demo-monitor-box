#include "stream/rtp_sender.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <vector>

#include "stream/h264_nal.h"

namespace mb {

namespace {
constexpr uint8_t kPayloadType = 96;
}

RtpSender::~RtpSender() { close(); }

bool RtpSender::open(const std::string& host, int port, int mtu, std::string* err) {
    close();
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        if (err) *err = "无效的 UDP 目标地址: " + host;
        return false;
    }
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        if (err) *err = std::string("socket: ") + strerror(errno);
        return false;
    }
    // 1080p IDR 约 100~200KB, 发送缓冲给大一些, 否则 loopback 上也会 ENOBUFS
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    memcpy(dst_, &addr, sizeof(addr));
    dst_len_ = sizeof(addr);
    mtu_ = mtu < 200 ? 200 : (mtu > 65000 ? 65000 : mtu);
    ssrc_ = (uint32_t)time(nullptr) ^ ((uint32_t)getpid() << 8);
    seq_ = (uint16_t)(ssrc_ & 0xFFFF);
    return true;
}

void RtpSender::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

void RtpSender::send_packet(const uint8_t* payload, size_t n, bool marker, uint32_t ts90) {
    uint8_t buf[65536];
    if (n + 12 > sizeof(buf)) return;
    buf[0] = 0x80;
    buf[1] = (uint8_t)((marker ? 0x80 : 0) | kPayloadType);
    buf[2] = (uint8_t)(seq_ >> 8);
    buf[3] = (uint8_t)(seq_ & 0xFF);
    buf[4] = (uint8_t)(ts90 >> 24);
    buf[5] = (uint8_t)(ts90 >> 16);
    buf[6] = (uint8_t)(ts90 >> 8);
    buf[7] = (uint8_t)ts90;
    buf[8] = (uint8_t)(ssrc_ >> 24);
    buf[9] = (uint8_t)(ssrc_ >> 16);
    buf[10] = (uint8_t)(ssrc_ >> 8);
    buf[11] = (uint8_t)ssrc_;
    memcpy(buf + 12, payload, n);
    ++seq_;
    const ssize_t r = sendto(fd_, buf, n + 12, MSG_DONTWAIT, (const sockaddr*)dst_, dst_len_);
    // 没人收时 loopback 会回 ECONNREFUSED, 这是常态, 不算错误
    if (r < 0 && errno != ECONNREFUSED)
        ++errors_;
    else
        ++packets_;
}

void RtpSender::send_access_unit(const uint8_t* data, size_t size, uint64_t ts_ms) {
    if (fd_ < 0) return;
    const uint32_t ts90 = (uint32_t)(ts_ms * 90);
    const std::vector<NalUnit> nals = split_annexb(data, size);
    const size_t max_payload = (size_t)mtu_ - 12;

    std::vector<uint8_t> frag(max_payload);
    for (size_t k = 0; k < nals.size(); ++k) {
        const NalUnit& u = nals[k];
        const bool last_nal = (k + 1 == nals.size());
        if (u.size <= max_payload) {
            send_packet(u.data, u.size, last_nal, ts90);
            continue;
        }
        // FU-A
        const uint8_t hdr = u.data[0];
        const uint8_t fu_ind = (uint8_t)((hdr & 0xE0) | 28);
        const uint8_t type = (uint8_t)(hdr & 0x1F);
        const uint8_t* p = u.data + 1;
        size_t remain = u.size - 1;
        bool first = true;
        while (remain > 0) {
            const size_t chunk = remain > max_payload - 2 ? max_payload - 2 : remain;
            const bool last = (chunk == remain);
            frag[0] = fu_ind;
            frag[1] = (uint8_t)((first ? 0x80 : 0) | (last ? 0x40 : 0) | type);
            memcpy(frag.data() + 2, p, chunk);
            send_packet(frag.data(), chunk + 2, last && last_nal, ts90);
            p += chunk;
            remain -= chunk;
            first = false;
        }
    }
}

}  // namespace mb
