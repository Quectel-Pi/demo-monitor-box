#include "stream/mp4_writer.h"

#include <cstdio>
#include <cstring>

#include "stream/h264_nal.h"

namespace mb {
namespace {

constexpr uint32_t kTimescale = 90000;

class Buf {
public:
    std::vector<uint8_t> b;

    void u8(uint32_t v) { b.push_back((uint8_t)v); }
    void u16(uint32_t v) {
        u8(v >> 8);
        u8(v);
    }
    void u32(uint32_t v) {
        u16(v >> 16);
        u16(v & 0xFFFF);
    }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void bytes(const void* p, size_t n) {
        const uint8_t* q = (const uint8_t*)p;
        b.insert(b.end(), q, q + n);
    }
    void fourcc(const char* s) { bytes(s, 4); }

    size_t begin(const char* type) {
        const size_t at = b.size();
        u32(0);
        fourcc(type);
        return at;
    }
    size_t begin_full(const char* type, uint32_t version, uint32_t flags) {
        const size_t at = begin(type);
        u32((version << 24) | (flags & 0xFFFFFF));
        return at;
    }
    void end(size_t at) {
        const uint32_t sz = (uint32_t)(b.size() - at);
        b[at] = (uint8_t)(sz >> 24);
        b[at + 1] = (uint8_t)(sz >> 16);
        b[at + 2] = (uint8_t)(sz >> 8);
        b[at + 3] = (uint8_t)sz;
    }
    void matrix() {
        static const uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
        for (uint32_t v : m) u32(v);
    }
};

struct Sample {
    uint32_t size = 0;
    uint32_t duration = 0;
    bool sync = false;
};

void build_moov(Buf* o, const std::vector<Sample>& samples, const std::vector<uint8_t>& sps,
                const std::vector<uint8_t>& pps, int width, int height, uint32_t mdat_data_offset) {
    uint64_t total = 0;
    for (const Sample& s : samples) total += s.duration;
    const uint32_t dur_ms = (uint32_t)(total * 1000 / kTimescale);

    const size_t moov = o->begin("moov");

    const size_t mvhd = o->begin_full("mvhd", 0, 0);
    o->u32(0);
    o->u32(0);
    o->u32(1000);
    o->u32(dur_ms);
    o->u32(0x00010000);
    o->u16(0x0100);
    o->zeros(10);
    o->matrix();
    o->zeros(24);
    o->u32(2);
    o->end(mvhd);

    const size_t trak = o->begin("trak");
    const size_t tkhd = o->begin_full("tkhd", 0, 3);
    o->u32(0);
    o->u32(0);
    o->u32(1);
    o->u32(0);
    o->u32(dur_ms);
    o->zeros(8);
    o->u16(0);
    o->u16(0);
    o->u16(0);
    o->u16(0);
    o->matrix();
    o->u32((uint32_t)width << 16);
    o->u32((uint32_t)height << 16);
    o->end(tkhd);

    const size_t mdia = o->begin("mdia");
    const size_t mdhd = o->begin_full("mdhd", 0, 0);
    o->u32(0);
    o->u32(0);
    o->u32(kTimescale);
    o->u32((uint32_t)total);
    o->u16(0x55C4);  // "und"
    o->u16(0);
    o->end(mdhd);

    const size_t hdlr = o->begin_full("hdlr", 0, 0);
    o->u32(0);
    o->fourcc("vide");
    o->zeros(12);
    o->bytes("VideoHandler", 13);
    o->end(hdlr);

    const size_t minf = o->begin("minf");
    const size_t vmhd = o->begin_full("vmhd", 0, 1);
    o->zeros(8);
    o->end(vmhd);
    const size_t dinf = o->begin("dinf");
    const size_t dref = o->begin_full("dref", 0, 0);
    o->u32(1);
    const size_t url = o->begin_full("url ", 0, 1);
    o->end(url);
    o->end(dref);
    o->end(dinf);

    const size_t stbl = o->begin("stbl");
    const size_t stsd = o->begin_full("stsd", 0, 0);
    o->u32(1);
    const size_t avc1 = o->begin("avc1");
    o->zeros(6);
    o->u16(1);
    o->zeros(16);
    o->u16((uint32_t)width);
    o->u16((uint32_t)height);
    o->u32(0x00480000);
    o->u32(0x00480000);
    o->u32(0);
    o->u16(1);
    o->zeros(32);
    o->u16(0x0018);
    o->u16(0xFFFF);
    const size_t avcc = o->begin("avcC");
    o->u8(1);
    o->u8(sps.size() > 1 ? sps[1] : 100);
    o->u8(sps.size() > 2 ? sps[2] : 0);
    o->u8(sps.size() > 3 ? sps[3] : 40);
    o->u8(0xFF);
    o->u8(0xE1);
    o->u16((uint32_t)sps.size());
    o->bytes(sps.data(), sps.size());
    o->u8(1);
    o->u16((uint32_t)pps.size());
    o->bytes(pps.data(), pps.size());
    o->end(avcc);
    o->end(avc1);
    o->end(stsd);

    // stts: 时长游程编码
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (const Sample& s : samples) {
        if (!runs.empty() && runs.back().second == s.duration)
            ++runs.back().first;
        else
            runs.push_back({1, s.duration});
    }
    const size_t stts = o->begin_full("stts", 0, 0);
    o->u32((uint32_t)runs.size());
    for (const auto& r : runs) {
        o->u32(r.first);
        o->u32(r.second);
    }
    o->end(stts);

    const size_t stss = o->begin_full("stss", 0, 0);
    uint32_t nsync = 0;
    for (const Sample& s : samples) nsync += s.sync ? 1 : 0;
    o->u32(nsync);
    for (size_t i = 0; i < samples.size(); ++i)
        if (samples[i].sync) o->u32((uint32_t)i + 1);
    o->end(stss);

    const size_t stsc = o->begin_full("stsc", 0, 0);
    o->u32(1);
    o->u32(1);
    o->u32((uint32_t)samples.size());
    o->u32(1);
    o->end(stsc);

    const size_t stsz = o->begin_full("stsz", 0, 0);
    o->u32(0);
    o->u32((uint32_t)samples.size());
    for (const Sample& s : samples) o->u32(s.size);
    o->end(stsz);

    const size_t stco = o->begin_full("stco", 0, 0);
    o->u32(1);
    o->u32(mdat_data_offset);
    o->end(stco);

    o->end(stbl);
    o->end(minf);
    o->end(mdia);
    o->end(trak);
    o->end(moov);
}

}  // namespace

bool write_mp4(const std::string& path, const std::vector<const EncodedPacket*>& packets, int width,
               int height, std::string* err) {
    std::vector<uint8_t> sps, pps;
    std::vector<Sample> samples;
    Buf mdat_payload;

    bool started = false;
    for (size_t i = 0; i < packets.size(); ++i) {
        const EncodedPacket* p = packets[i];
        const std::vector<NalUnit> nals = split_annexb(p->data.data(), p->data.size());
        const bool idr = has_idr(nals);
        if (!started && !idr) continue;
        started = true;

        Sample s;
        s.sync = idr;
        const size_t before = mdat_payload.b.size();
        for (const NalUnit& u : nals) {
            const int t = u.type();
            if (t == 7) {
                if (sps.empty()) sps.assign(u.data, u.data + u.size);
                continue;
            }
            if (t == 8) {
                if (pps.empty()) pps.assign(u.data, u.data + u.size);
                continue;
            }
            if (t == 9 || t == 6) continue;  // AUD / SEI 不进 mp4
            mdat_payload.u32((uint32_t)u.size);
            mdat_payload.bytes(u.data, u.size);
        }
        s.size = (uint32_t)(mdat_payload.b.size() - before);
        if (s.size == 0) continue;

        // 时长用下一帧时间戳差; 最后一帧沿用上一帧的
        uint32_t dur = 0;
        if (i + 1 < packets.size() && packets[i + 1]->ts_ms > p->ts_ms)
            dur = (uint32_t)((packets[i + 1]->ts_ms - p->ts_ms) * (kTimescale / 1000));
        if (dur == 0) dur = samples.empty() ? kTimescale / 30 : samples.back().duration;
        s.duration = dur;
        samples.push_back(s);
    }

    if (samples.empty() || sps.empty() || pps.empty()) {
        if (err) *err = "没有可用的 IDR/SPS/PPS, 无法封装 MP4";
        return false;
    }

    Buf ftyp;
    const size_t f = ftyp.begin("ftyp");
    ftyp.fourcc("isom");
    ftyp.u32(0x200);
    ftyp.fourcc("isom");
    ftyp.fourcc("iso2");
    ftyp.fourcc("avc1");
    ftyp.fourcc("mp41");
    ftyp.end(f);

    // moov 大小与 stco 的值无关, 先拿 0 算一遍尺寸再填真实偏移
    Buf moov;
    build_moov(&moov, samples, sps, pps, width, height, 0);
    const uint32_t data_off = (uint32_t)(ftyp.b.size() + moov.b.size() + 8);
    moov.b.clear();
    build_moov(&moov, samples, sps, pps, width, height, data_off);

    const std::string tmp = path + ".part";
    FILE* fp = fopen(tmp.c_str(), "wb");
    if (!fp) {
        if (err) *err = "无法写 " + tmp;
        return false;
    }
    uint8_t mdat_hdr[8];
    const uint32_t mdat_size = (uint32_t)(mdat_payload.b.size() + 8);
    mdat_hdr[0] = (uint8_t)(mdat_size >> 24);
    mdat_hdr[1] = (uint8_t)(mdat_size >> 16);
    mdat_hdr[2] = (uint8_t)(mdat_size >> 8);
    mdat_hdr[3] = (uint8_t)mdat_size;
    memcpy(mdat_hdr + 4, "mdat", 4);

    bool ok = fwrite(ftyp.b.data(), 1, ftyp.b.size(), fp) == ftyp.b.size() &&
              fwrite(moov.b.data(), 1, moov.b.size(), fp) == moov.b.size() &&
              fwrite(mdat_hdr, 1, 8, fp) == 8 &&
              fwrite(mdat_payload.b.data(), 1, mdat_payload.b.size(), fp) == mdat_payload.b.size();
    ok = (fclose(fp) == 0) && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        if (err) *err = "写 MP4 失败: " + path;
        return false;
    }
    return true;
}

}  // namespace mb
