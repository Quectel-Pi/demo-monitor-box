#include "video/v4l2_source.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "infer/jpeg_io.h"
#include "video/mpp_jpeg_decoder.h"

namespace mb {
namespace {

constexpr int kMaxBuffers = 8;

std::string fourcc_to_string(uint32_t f) {
    if (f == 0) return "?";
    char s[5];
    s[0] = (char)(f & 0xFF);
    s[1] = (char)((f >> 8) & 0xFF);
    s[2] = (char)((f >> 16) & 0xFF);
    s[3] = (char)((f >> 24) & 0xFF);
    s[4] = '\0';
    return s;
}

uint32_t string_to_fourcc(const std::string& s) {
    if (s.size() != 4) return 0;
    return v4l2_fourcc(s[0], s[1], s[2], s[3]);
}

std::string ioctl_error(const char* what) {
    return std::string(what) + " 失败: " + strerror(errno);
}

// 摄像头节点同时具备 V4L2_CAP_VIDEO_CAPTURE 与 STREAMING 才算采集节点;
// 同一个 UVC 设备还会冒出 metadata 节点 (如 /dev/video74), 不能用。
bool is_capture_device(int fd) {
    v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) return false;
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
}

// 与 v4l2-ctl 一致的控件名: 小写, 非字母数字变 '_', 合并连续的 '_', 去掉首尾 '_'。
// "Exposure, Dynamic Framerate" -> "exposure_dynamic_framerate"
std::string control_key(const char* name) {
    std::string out;
    for (const char* p = name; *p; ++p) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out.push_back(c);
        } else if (c >= 'A' && c <= 'Z') {
            out.push_back((char)(c - 'A' + 'a'));
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

}  // namespace

V4l2Source::V4l2Source() = default;
V4l2Source::~V4l2Source() { close(); }

std::string V4l2Source::find_uvc_device() {
    std::string fallback;
    for (int i = 0; i < 128; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        const int fd = ::open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        const bool uvc = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
                         strcmp((const char*)cap.driver, "uvcvideo") == 0 && is_capture_device(fd);
        bool mjpg = false;
        if (uvc) {
            v4l2_fmtdesc fdesc;
            memset(&fdesc, 0, sizeof(fdesc));
            fdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            for (fdesc.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fdesc) == 0; ++fdesc.index) {
                if (fdesc.pixelformat == v4l2_fourcc('M', 'J', 'P', 'G')) mjpg = true;
            }
        }
        ::close(fd);
        if (uvc && mjpg) return path;
        if (uvc && fallback.empty()) fallback = path;
    }
    return fallback;
}

void V4l2Source::apply_controls(const std::vector<std::pair<std::string, int>>& controls,
                                std::vector<std::string>* warnings) {
    if (fd_ < 0 || controls.empty()) return;
    std::vector<std::pair<std::string, uint32_t>> known;  // 名字 -> 控件 id
    v4l2_queryctrl q;
    memset(&q, 0, sizeof(q));
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (ioctl(fd_, VIDIOC_QUERYCTRL, &q) == 0) {
        if (!(q.flags & V4L2_CTRL_FLAG_DISABLED) && q.type != V4L2_CTRL_TYPE_CTRL_CLASS)
            known.emplace_back(control_key((const char*)q.name), q.id);
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    for (const auto& kv : controls) {
        uint32_t id = 0;
        for (const auto& k : known) {
            if (k.first == kv.first) {
                id = k.second;
                break;
            }
        }
        if (id == 0) {
            if (warnings) warnings->push_back(kv.first + ": 这个摄像头没有该控件");
            continue;
        }
        v4l2_control c;
        c.id = id;
        c.value = kv.second;
        if (ioctl(fd_, VIDIOC_S_CTRL, &c) < 0 && warnings)
            warnings->push_back(kv.first + "=" + std::to_string(kv.second) + ": " + strerror(errno));
    }
}

const char* V4l2Source::pixel_format_name() const {
    static thread_local std::string s;
    s = fourcc_to_string(pixel_format_);
    return s.c_str();
}

std::vector<std::string> V4l2Source::list_capture_devices(std::string* err) {
    std::vector<std::string> out;
    for (int i = 0; i < 96; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        const int fd = ::open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        if (!is_capture_device(fd)) {
            ::close(fd);
            continue;
        }

        v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        ioctl(fd, VIDIOC_QUERYCAP, &cap);

        // 挑一个最像"能用"的格式: 优先 MJPG, 分辨率取该格式的最大值。
        std::string fmt_desc = "?";
        v4l2_fmtdesc fdesc;
        memset(&fdesc, 0, sizeof(fdesc));
        fdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        uint32_t best_fmt = 0;
        int best_w = 0, best_h = 0;
        for (fdesc.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fdesc) == 0; ++fdesc.index) {
            int w = 0, h = 0;
            v4l2_frmsizeenum fs;
            memset(&fs, 0, sizeof(fs));
            fs.pixel_format = fdesc.pixelformat;
            fs.index = 0;
            while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0) {
                if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                    // 每帧尺寸单独比: 不能拿别的格式的结果比, 否则会串味
                    if ((int)fs.discrete.width * (int)fs.discrete.height > w * h) {
                        w = (int)fs.discrete.width;
                        h = (int)fs.discrete.height;
                    }
                }
                ++fs.index;
            }
            const bool is_mjpg = fdesc.pixelformat == v4l2_fourcc('M', 'J', 'P', 'G');
            const bool best_is_mjpg = best_fmt == v4l2_fourcc('M', 'J', 'P', 'G');
            if (best_fmt == 0 || (is_mjpg && !best_is_mjpg) ||
                (is_mjpg == best_is_mjpg && w * h > best_w * best_h)) {
                best_fmt = fdesc.pixelformat;
                best_w = w;
                best_h = h;
            }
        }
        if (best_fmt != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%dx%d %s", best_w, best_h, fourcc_to_string(best_fmt).c_str());
            fmt_desc = buf;
        }

        char line[256];
        snprintf(line, sizeof(line), "%s (%s) %s", path, (const char*)cap.card, fmt_desc.c_str());
        out.emplace_back(line);
        ::close(fd);
    }
    if (out.empty() && err) *err = "没有找到支持 STREAMING 的视频采集节点";
    return out;
}

bool V4l2Source::set_format(int width, int height, const std::string& pixel_format,
                            std::string* err) {
    v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // 先问驱动当前是什么, 作为缺省值。
    if (ioctl(fd_, VIDIOC_G_FMT, &fmt) < 0) {
        if (err) *err = ioctl_error("VIDIOC_G_FMT");
        return false;
    }
    if (width > 0 && height > 0) {
        fmt.fmt.pix.width = (uint32_t)width;
        fmt.fmt.pix.height = (uint32_t)height;
    }
    if (!pixel_format.empty()) {
        const uint32_t want = string_to_fourcc(pixel_format);
        if (want == 0) {
            if (err) *err = "pixel_format 必须是 4 个字符, 如 MJPG / YUYV";
            return false;
        }
        fmt.fmt.pix.pixelformat = want;
    } else {
        // 没指定就优先 MJPG: USB 3.0 上 1080p 只有 MJPG 能跑满 30fps。
        fmt.fmt.pix.pixelformat = v4l2_fourcc('M', 'J', 'P', 'G');
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            memset(&fmt, 0, sizeof(fmt));
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            fmt.fmt.pix.width = (uint32_t)width;
            fmt.fmt.pix.height = (uint32_t)height;
            fmt.fmt.pix.pixelformat = v4l2_fourcc('Y', 'U', 'Y', 'V');
            if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
                if (err) *err = ioctl_error("VIDIOC_S_FMT (MJPG/YUYV 都不支持)");
                return false;
            }
            width_ = (int)fmt.fmt.pix.width;
            height_ = (int)fmt.fmt.pix.height;
            pixel_format_ = fmt.fmt.pix.pixelformat;
            return true;
        }
        width_ = (int)fmt.fmt.pix.width;
        height_ = (int)fmt.fmt.pix.height;
        pixel_format_ = fmt.fmt.pix.pixelformat;
        return true;
    }

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        if (err) {
            *err = ioctl_error("VIDIOC_S_FMT") + " (格式 " + pixel_format + " " +
                   std::to_string(width) + "x" + std::to_string(height) + ")";
        }
        return false;
    }
    // 驱动可能悄悄改尺寸, 以它返回的为准 —— 后面 letterbox 全靠这个。
    width_ = (int)fmt.fmt.pix.width;
    height_ = (int)fmt.fmt.pix.height;
    pixel_format_ = fmt.fmt.pix.pixelformat;
    return true;
}

bool V4l2Source::set_fps(int fps, std::string* err) {
    if (fps <= 0) return true;
    v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_G_PARM, &parm) < 0) return true;  // 不支持就算了, 不是致命错
    if (!(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) return true;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = (uint32_t)fps;
    if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
        if (err) *err = ioctl_error("VIDIOC_S_PARM") + " (忽略, 用驱动默认帧率)";
        err->clear();  // 帧率设不上不算失败, 调用方打日志就行
        return true;
    }
    return true;
}

bool V4l2Source::start_streaming(std::string* err) {
    v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = (uint32_t)(buffers_.empty() ? 4 : buffers_.size());
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        if (err) *err = ioctl_error("VIDIOC_REQBUFS");
        return false;
    }
    if (req.count < 2) {
        if (err) *err = "驱动只给了 " + std::to_string(req.count) + " 个 buffer (至少要 2 个)";
        return false;
    }

    buffers_.assign(req.count, Buffer{});
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            if (err) *err = ioctl_error("VIDIOC_QUERYBUF");
            return false;
        }
        void* p = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
        if (p == MAP_FAILED) {
            if (err) *err = ioctl_error("mmap");
            return false;
        }
        buffers_[i].start = p;
        buffers_[i].length = buf.length;

        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            if (err) *err = ioctl_error("VIDIOC_QBUF");
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        if (err) *err = ioctl_error("VIDIOC_STREAMON");
        return false;
    }
    streaming_ = true;
    return true;
}

bool V4l2Source::open(const std::string& device, int width, int height, int fps,
                      const std::string& pixel_format, int buffers, std::string* err) {
    close();
    device_ = device;

    fd_ = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        if (err) *err = "打开 " + device + " 失败: " + strerror(errno);
        return false;
    }
    if (!is_capture_device(fd_)) {
        if (err) {
            *err = device + " 不是视频采集节点 (可能是 metadata 节点, 用 --list 挑采集节点)";
        }
        close();
        return false;
    }

    if (!set_format(width, height, pixel_format, err)) {
        close();
        return false;
    }
    // set_fps 的 err 只是提示, 不阻断。
    std::string fps_warn;
    set_fps(fps, &fps_warn);
    if (!fps_warn.empty()) printf("[video] %s\n", fps_warn.c_str());

    // buffers_ 此时为空, 用参数告诉 start_streaming 要几个。
    int want = buffers > 0 ? buffers : 4;
    if (want < 2) want = 2;
    if (want > kMaxBuffers) want = kMaxBuffers;
    buffers_.assign((size_t)want, Buffer{});

    if (!start_streaming(err)) {
        close();
        return false;
    }

    printf("[video] %s %dx%d %s fps<=%d buffers=%zu\n", device.c_str(), width_, height_,
           pixel_format_name(), fps, buffers_.size());
    if (pixel_format_ == v4l2_fourcc('M', 'J', 'P', 'G') && hardware_jpeg_enabled_) {
        std::unique_ptr<MppJpegDecoder> decoder(new MppJpegDecoder());
        std::string jerr;
        if (decoder->open(width_, height_, &jerr)) {
            jpeg_decoder_ = std::move(decoder);
            printf("[video] JPEG 解码: MPP 硬件 + RGA (%dx%d)\n", width_, height_);
        } else {
            fprintf(stderr, "[video] MPP JPEG 不可用, 回退 CPU: %s\n", jerr.c_str());
        }
    }
    return true;
}

void V4l2Source::close() {
    if (fd_ >= 0) {
        if (streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for (Buffer& b : buffers_) {
            if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
        }
    }
    buffers_.clear();
    jpeg_decoder_.reset();
    hardware_jpeg_failures_ = 0;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool V4l2Source::read(VideoFrame* out, int timeout_ms, std::string* err) {
    if (!streaming_) {
        if (err) *err = "采集没有启动";
        return false;
    }
    pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) return false;
        if (err) *err = ioctl_error("poll");
        return false;
    }
    if (pr == 0) return false;  // 超时, 不是错误

    v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return false;
        if (err) *err = ioctl_error("VIDIOC_DQBUF");
        return false;
    }
    if (buf.index >= buffers_.size()) {
        if (err) *err = "驱动返回越界 buffer index";
        return false;
    }
    // 取空队列, 只留最新的一帧 (见头文件)
    for (;;) {
        pollfd more{fd_, POLLIN, 0};
        if (poll(&more, 1, 0) <= 0 || !(more.revents & POLLIN)) break;
        v4l2_buffer next;
        memset(&next, 0, sizeof(next));
        next.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        next.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &next) < 0 || next.index >= buffers_.size()) break;
        ioctl(fd_, VIDIOC_QBUF, &buf);
        buf = next;
        ++stale_skipped_;
    }

    if (buf.flags & V4L2_BUF_FLAG_ERROR) {
        // 坏的 buffer 必须还回去, 否则几个之后就没 buffer 可用了。
        // USB 偶发的坏帧不是设备故障, 当作"这次没取到帧"处理, 不报错。
        ioctl(fd_, VIDIOC_QBUF, &buf);
        ++error_frames_;
        return false;
    }

    if (out) {
        out->data.resize(buf.bytesused);
        if (buf.bytesused) {
            memcpy(out->data.data(), buffers_[buf.index].start, buf.bytesused);
        }
        out->width = width_;
        out->height = height_;
        out->pixel_format = pixel_format_;
        out->sequence = buf.sequence;
        out->timestamp_ms = (uint64_t)buf.timestamp.tv_sec * 1000u +
                            (uint64_t)(buf.timestamp.tv_usec / 1000);
    }
    ioctl(fd_, VIDIOC_QBUF, &buf);
    return true;
}

void yuyv_to_rgb(const uint8_t* src, int width, int height, ImageRGB* out) {
    out->width = width;
    out->height = height;
    out->data.resize(out->size_bytes());
    uint8_t* dst = out->data.data();

    for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + (size_t)y * width * 2;
        uint8_t* d = dst + (size_t)y * width * 3;
        for (int x = 0; x < width; x += 2, s += 4, d += 6) {
            const int y0 = s[0], u = s[1], y1 = s[2], v = s[3];
            const int c = y0 - 16, c1 = y1 - 16;
            const int d_ = u - 128, e = v - 128;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d_ - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d_ + 128) >> 8;
            d[0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            d[1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            d[2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
            r = (298 * c1 + 409 * e + 128) >> 8;
            g = (298 * c1 - 100 * d_ - 208 * e + 128) >> 8;
            b = (298 * c1 + 516 * d_ + 128) >> 8;
            d[3] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            d[4] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            d[5] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
}

bool V4l2Source::decode_frame(const VideoFrame& frame, ImageRGB* out, std::string* err) {
    if (frame.pixel_format == v4l2_fourcc('M', 'J', 'P', 'G')) {
        if (jpeg_decoder_) {
            if (jpeg_decoder_->decode(frame.data.data(), frame.data.size(), out, err)) return true;
            ++hardware_jpeg_failures_;
            const std::string herr = err ? *err : "unknown error";
            fprintf(stderr, "[video] MPP JPEG 解码失败, 本帧回退 CPU: %s\n", herr.c_str());
            // A poll/enqueue failure means the hardware task pipeline may still own
            // frame buffers. Destroy it immediately instead of leaving the capture
            // thread permanently wedged. Bad JPEG payloads use the normal retry path.
            const bool task_stalled =
                herr.find("poll") != std::string::npos ||
                herr.find("enqueue") != std::string::npos ||
                herr.find("dequeue") != std::string::npos;
            if (task_stalled || hardware_jpeg_failures_ >= 3) {
                fprintf(stderr, "[video] MPP JPEG 任务异常, 改用 CPU 解码\n");
                jpeg_decoder_.reset();
            }
        }
        if (err) err->clear();
        return jpeg_decode_mem_fast(frame.data.data(), frame.data.size(), out, err);
    }
    return frame_to_rgb(frame, out, err);
}

bool frame_to_rgb(const VideoFrame& frame, ImageRGB* out, std::string* err) {
    if (frame.pixel_format == v4l2_fourcc('M', 'J', 'P', 'G')) {
        return jpeg_decode_mem_fast(frame.data.data(), frame.data.size(), out, err);
    }
    if (frame.pixel_format == v4l2_fourcc('Y', 'U', 'Y', 'V')) {
        if (frame.width <= 0 || frame.height <= 0 ||
            frame.data.size() < (size_t)frame.width * frame.height * 2) {
            if (err) *err = "YUYV 帧长度不足";
            return false;
        }
        yuyv_to_rgb(frame.data.data(), frame.width, frame.height, out);
        return true;
    }
    if (err) *err = "不支持的像素格式: " + fourcc_to_string(frame.pixel_format);
    return false;
}

}  // namespace mb
