#include "infer/jpeg_io.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <jpeglib.h>
}

namespace mb {
namespace {

// libjpeg 的默认 error_exit 会 exit()。这里换成 longjmp, 让坏图/截断图不至于把整个
// 监控进程带走 —— UVC 采到的 MJPG 帧偶发截断是常态。
// 注意: longjmp 会跳过 C++ 析构, 所以 jmp 缓冲区里只放 POD, 出错信息用裸 char 数组带出,
// std::string 只在 setjmp 返回非 0 之后才构造。
struct JpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX];
};

void jpeg_on_error(j_common_ptr cinfo) {
    JpegErrorMgr* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->msg);
    longjmp(err->jb, 1);
}

bool read_whole_file(const std::string& path, std::vector<uint8_t>* out, std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "open failed: " + path;
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (err) *err = "seek failed: " + path;
        return false;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        if (err) *err = "tell failed: " + path;
        return false;
    }
    rewind(f);
    out->resize((size_t)len);
    size_t got = len > 0 ? fread(out->data(), 1, (size_t)len, f) : 0;
    fclose(f);
    if (got != (size_t)len) {
        if (err) *err = "short read: " + path;
        return false;
    }
    return true;
}

bool jpeg_decode_mem_impl(const uint8_t* buf, size_t len, ImageRGB* out, std::string* err,
                          bool realtime) {
    if (!buf || len == 0 || !out) {
        if (err) *err = "empty input";
        return false;
    }

    jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    jerr.msg[0] = '\0';

    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        if (err) *err = std::string("jpeg decode: ") + (jerr.msg[0] ? jerr.msg : "unknown error");
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<unsigned char*>(buf), (unsigned long)len);
    jpeg_read_header(&cinfo, TRUE);

    // 统一转成 RGB888, 省掉调用方再做一次转换。
    cinfo.out_color_space = JCS_RGB;
    if (realtime) {
        // MPP JPEG output is not coherent when RGA writes into the CPU-owned
        // ImageRGB buffer (stale OSD pixels survive into the next frame). Keep
        // libjpeg as the correctness-first live path and use its fast DCT/upsample
        // kernels to recover enough headroom for 1080p30.
        cinfo.dct_method = JDCT_IFAST;
        cinfo.do_fancy_upsampling = FALSE;
    }
    jpeg_start_decompress(&cinfo);

    const int w = (int)cinfo.output_width;
    const int h = (int)cinfo.output_height;
    const int comps = (int)cinfo.output_components;
    if (w <= 0 || h <= 0 || comps != 3) {
        jpeg_destroy_decompress(&cinfo);
        if (err) *err = "unexpected jpeg layout";
        return false;
    }

    out->width = w;
    out->height = h;
    out->data.resize((size_t)w * h * 3);

    // Decode directly into the tightly packed output image. The old path
    // allocated a row buffer and memcpy'd every one of the 1080 rows.
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rows[1];
        rows[0] = out->row((int)cinfo.output_scanline);
        jpeg_read_scanlines(&cinfo, rows, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

}  // namespace

bool jpeg_decode_mem(const uint8_t* buf, size_t len, ImageRGB* out, std::string* err) {
    return jpeg_decode_mem_impl(buf, len, out, err, false);
}

bool jpeg_decode_mem_fast(const uint8_t* buf, size_t len, ImageRGB* out, std::string* err) {
    return jpeg_decode_mem_impl(buf, len, out, err, true);
}

bool jpeg_decode_file(const std::string& path, ImageRGB* out, std::string* err) {
    std::vector<uint8_t> file;
    if (!read_whole_file(path, &file, err)) return false;
    return jpeg_decode_mem(file.data(), file.size(), out, err);
}

bool jpeg_encode_mem(const ImageRGB& img, int quality, std::vector<uint8_t>* out, std::string* err) {
    if (img.width <= 0 || img.height <= 0 || img.data.size() < img.size_bytes()) {
        if (err) *err = "bad image";
        return false;
    }

    jpeg_compress_struct cinfo;
    JpegErrorMgr jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    jerr.msg[0] = '\0';

    unsigned char* volatile mem = nullptr;
    unsigned long volatile mem_size = 0;

    if (setjmp(jerr.jb)) {
        jpeg_destroy_compress(&cinfo);
        free(mem);
        if (err) *err = std::string("jpeg encode: ") + (jerr.msg[0] ? jerr.msg : "unknown error");
        return false;
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, const_cast<unsigned char**>(&mem), const_cast<unsigned long*>(&mem_size));

    cinfo.image_width = (JDIMENSION)img.width;
    cinfo.image_height = (JDIMENSION)img.height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW rows[1];
        rows[0] = const_cast<JSAMPROW>(img.row((int)cinfo.next_scanline));
        jpeg_write_scanlines(&cinfo, rows, 1);
    }

    jpeg_finish_compress(&cinfo);
    out->assign(mem, mem + mem_size);
    jpeg_destroy_compress(&cinfo);
    free(mem);
    return true;
}

bool jpeg_encode_file(const std::string& path, const ImageRGB& img, int quality, std::string* err) {
    std::vector<uint8_t> buf;
    if (!jpeg_encode_mem(img, quality, &buf, err)) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        if (err) *err = "open for write failed: " + path;
        return false;
    }
    const size_t n = fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (n != buf.size()) {
        if (err) *err = "short write: " + path;
        return false;
    }
    return true;
}

}  // namespace mb
