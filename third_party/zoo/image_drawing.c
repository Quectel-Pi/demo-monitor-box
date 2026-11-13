#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "image_drawing.h"
#include "font.h"

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))

static int clamp(float val, int min, int max)
{
    return val > min ? (val < max ? val : max) : min;
}

// src color format(ARGB888) To dest format color
static unsigned int convert_color(unsigned int src_color, image_format_t dst_fmt)
{
    // printf("sizeof(int)=%d\n", sizeof(int));
    unsigned int dst_color = 0x0;
    unsigned char* p_src_color = (unsigned char*)&src_color;
    unsigned char* p_dst_color = (unsigned char*)&dst_color;
    char r = p_src_color[2];
    char g = p_src_color[1];
    char b = p_src_color[0];
    char a = p_src_color[3];

    switch (dst_fmt)
    {
    case IMAGE_FORMAT_GRAY8:
        p_dst_color[0] = a;
        break;
    case IMAGE_FORMAT_RGB888:
        p_dst_color[0] = r;
        p_dst_color[1] = g;
        p_dst_color[2] = b;
        break;
    case IMAGE_FORMAT_RGBA8888:
        p_dst_color[0] = r;
        p_dst_color[1] = g;
        p_dst_color[2] = b;
        p_dst_color[3] = a;
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
        p_dst_color[0] = (0.299 * r + 0.587 * g + 0.114 * b);
        p_dst_color[1] = clamp(128 + (-0.14713 * r - 0.28886 * g + 0.436 * b), 0, 255);
        p_dst_color[2] = clamp(128 + (0.615 * r - 0.51499 * g - 0.10001 * b), 0, 255);
        break;
    case IMAGE_FORMAT_YUV420SP_NV21:
        p_dst_color[0] = 0.299 * r + 0.587 * g + 0.114 * b;
        p_dst_color[1] = 0.877 * (r - p_dst_color[0]);
        p_dst_color[2] = 0.492 * (b - p_dst_color[0]);
        break;
    default:
        break;
    }
    return dst_color;
}

static void draw_rectangle_c1(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    if (thickness == -1) {
        // filled
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx; x < rx + rw; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x] = pen_color[0];
            }
        }

        return;
    }

    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // draw top
    {
        for (int y = ry - t0; y < ry + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x] = pen_color[0];
            }
        }
    }

    // draw bottom
    {
        for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x] = pen_color[0];
            }
        }
    }

    // draw left
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x] = pen_color[0];
        }
    }

    // draw right
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x] = pen_color[0];
        }
    }
}

static void draw_rectangle_c2(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    if (thickness == -1) {
        // filled
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx; x < rx + rw; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }

        return;
    }

    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // draw top
    {
        for (int y = ry - t0; y < ry + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
    }

    // draw bottom
    {
        for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
    }

    // draw left
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 2 + 0] = pen_color[0];
            p[x * 2 + 1] = pen_color[1];
        }
    }

    // draw right
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 2 + 0] = pen_color[0];
            p[x * 2 + 1] = pen_color[1];
        }
    }
}

static void draw_rectangle_c3(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;

    if (thickness == -1) {
        // filled
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx; x < rx + rw; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }

        return;
    }

    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // draw top
    {
        for (int y = ry - t0; y < ry + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }
    }

    // draw bottom
    {
        for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }
    }

    // draw left
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 3 + 0] = pen_color[0];
            p[x * 3 + 1] = pen_color[1];
            p[x * 3 + 2] = pen_color[2];
        }
    }

    // draw right
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 3 + 0] = pen_color[0];
            p[x * 3 + 1] = pen_color[1];
            p[x * 3 + 2] = pen_color[2];
        }
    }
}

static void draw_rectangle_c4(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 4;

    if (thickness == -1) {
        // filled
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx; x < rx + rw; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }

        return;
    }

    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // draw top
    {
        for (int y = ry - t0; y < ry + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
    }

    // draw bottom
    {
        for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
    }

    // draw left
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 4 + 0] = pen_color[0];
            p[x * 4 + 1] = pen_color[1];
            p[x * 4 + 2] = pen_color[2];
            p[x * 4 + 3] = pen_color[3];
        }
    }

    // draw right
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 4 + 0] = pen_color[0];
            p[x * 4 + 1] = pen_color[1];
            p[x * 4 + 2] = pen_color[2];
            p[x * 4 + 3] = pen_color[3];
        }
    }
}

static void draw_rectangle_yuv420sp(unsigned char* yuv420sp, int w, int h, int rx, int ry, int rw, int rh,
                                    unsigned int color, int thickness)
{
    // assert w % 2 == 0
    // assert h % 2 == 0
    // assert rx % 2 == 0
    // assert ry % 2 == 0
    // assert rw % 2 == 0
    // assert rh % 2 == 0
    // assert thickness % 2 == 0

    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    unsigned char* Y = yuv420sp;
    draw_rectangle_c1(Y, w, h, rx, ry, rw, rh, v_y, thickness);

    unsigned char* UV = yuv420sp + w * h;
    int thickness_uv = thickness == -1 ? thickness : max(thickness / 2, 1);
    draw_rectangle_c2(UV, w / 2, h / 2, rx / 2, ry / 2, rw / 2, rh / 2, v_uv, thickness_uv);
}

static inline int distance_lessequal(int x0, int y0, int x1, int y1, float r)
{
    int dx = x0 - x1;
    int dy = y0 - y1;
    int q = dx * dx + dy * dy;
    return q <= r * r;
}

static inline int distance_inrange(int x0, int y0, int x1, int y1, float r0, float r1)
{
    int dx = x0 - x1;
    int dy = y0 - y1;
    int q = dx * dx + dy * dy;
    return q >= r0 * r0 && q < r1 * r1;
}

static void draw_circle_c1(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    if (thickness == -1) {
        // filled
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                // distance from cx cy
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x] = pen_color[0];
                }
            }
        }

        return;
    }

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - (radius - 1) - t0; y < cy + radius + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = cx - (radius - 1) - t0; x < cx + radius + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from cx cy
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x] = pen_color[0];
            }
        }
    }
}

static void draw_circle_c2(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    if (thickness == -1) {
        // filled
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                // distance from cx cy
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x * 2 + 0] = pen_color[0];
                    p[x * 2 + 1] = pen_color[1];
                }
            }
        }

        return;
    }

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - radius - t0; y < cy + radius + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = cx - radius - t0; x < cx + radius + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from cx cy
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
    }
}

static void draw_circle_c3(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;

    if (thickness == -1) {
        // filled
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                // distance from cx cy
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x * 3 + 0] = pen_color[0];
                    p[x * 3 + 1] = pen_color[1];
                    p[x * 3 + 2] = pen_color[2];
                }
            }
        }

        return;
    }

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - radius - t0; y < cy + radius + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = cx - radius - t0; x < cx + radius + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from cx cy
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }
    }
}

static void draw_circle_c4(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 4;

    if (thickness == -1) {
        // filled
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                // distance from cx cy
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x * 4 + 0] = pen_color[0];
                    p[x * 4 + 1] = pen_color[1];
                    p[x * 4 + 2] = pen_color[2];
                    p[x * 4 + 3] = pen_color[3];
                }
            }
        }

        return;
    }

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - (radius - 1) - t0; y < cy + radius + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = cx - (radius - 1) - t0; x < cx + radius + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from cx cy
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
    }
}

static void draw_circle_yuv420sp(unsigned char* yuv420sp, int w, int h, int cx, int cy, int radius, unsigned int color,
                                 int thickness)
{
    // assert w % 2 == 0
    // assert h % 2 == 0
    // assert cx % 2 == 0
    // assert cy % 2 == 0
    // assert radius % 2 == 0
    // assert thickness % 2 == 0

    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    unsigned char* Y = yuv420sp;
    draw_circle_c1(Y, w, h, cx, cy, radius, v_y, thickness);

    unsigned char* UV = yuv420sp + w * h;
    int thickness_uv = thickness == -1 ? thickness : max(thickness / 2, 1);
    draw_circle_c2(UV, w / 2, h / 2, cx / 2, cy / 2, radius / 2, v_uv, thickness_uv);
}

static inline int distance_lessthan(int x, int y, int x0, int y0, int x1, int y1, float t)
{
    int dx01 = x1 - x0;
    int dy01 = y1 - y0;
    int dx0 = x - x0;
    int dy0 = y - y0;

    float r = (float)(dx0 * dx01 + dy0 * dy01) / (dx01 * dx01 + dy01 * dy01);

    if (r < 0 || r > 1)
        return 0;

    float px = x0 + dx01 * r;
    float py = y0 + dy01 * r;
    float dx = x - px;
    float dy = y - py;
    float p = dx * dx + dy * dy;
    return p < t;
}

static void draw_line_c1(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from line
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x] = pen_color[0];
            }
        }
    }
}

static void draw_line_c2(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from line
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
    }
}

static void draw_line_c3(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    const int stride = w * 3;
    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // The former implementation scanned the line's whole bounding rectangle and
    // tested every pixel against the line equation. A diagonal spanning the
    // 1080p frame therefore scanned hundreds of thousands of pixels. Walk the
    // line once and stamp a small thickness brush instead.
    int x = x0;
    int y = y0;
    const int dx = abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        for (int oy = -t0; oy < t1; ++oy) {
            const int py = y + oy;
            if (py < 0 || py >= h) continue;
            unsigned char* row = pixels + (size_t)py * stride;
            for (int ox = -t0; ox < t1; ++ox) {
                const int px = x + ox;
                if (px < 0 || px >= w) continue;
                row[px * 3 + 0] = pen_color[0];
                row[px * 3 + 1] = pen_color[1];
                row[px * 3 + 2] = pen_color[2];
            }
        }
        if (x == x1 && y == y1) break;
        const int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
}

static void draw_line_c4(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 4;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from line
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
    }
}

static void draw_line_yuv420sp(unsigned char* yuv420sp, int w, int h, int x0, int y0, int x1, int y1,
                               unsigned int color, int thickness)
{
    // assert w % 2 == 0
    // assert h % 2 == 0
    // assert x0 % 2 == 0
    // assert y0 % 2 == 0
    // assert x1 % 2 == 0
    // assert y1 % 2 == 0
    // assert thickness % 2 == 0

    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    unsigned char* Y = yuv420sp;
    draw_line_c1(Y, w, h, x0, y0, x1, y1, v_y, thickness);

    unsigned char* UV = yuv420sp + w * h;
    int thickness_uv = thickness == -1 ? thickness : max(thickness / 2, 1);
    draw_line_c2(UV, w / 2, h / 2, x0 / 2, y0 / 2, x1 / 2, y1 / 2, v_uv, thickness_uv);
}

int text_width(const char* text, int fontsize)
{
    if (text == NULL || fontsize < 1) return 0;

    int line_width = 0;
    int max_width = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        if (*p == '\n') {
            max_width = max(max_width, line_width);
            line_width = 0;
        } else if (*p >= 32 && *p <= 126) {
            const int index = (int)(*p - 32);
            line_width += (ui_font_advance[index] * fontsize * 2 + UI_FONT_EM / 2) / UI_FONT_EM;
        }
    }
    return max(max_width, line_width);
}

int text_height(int fontsize)
{
    return fontsize < 1 ? 0 : fontsize * 2;
}

// Bilinearly sample one glyph cell. The atlas is kept at a comfortable
// rendering size and scaled at draw time so OSD text stays sharp from the
// compact diagnostics line through the larger alarm banner.
static unsigned char sample_glyph(int index, int tx, int ty, int target_width,
                                  int target_height)
{
    const unsigned char* glyph = ui_font_data[index];
    float sx = ((float)tx + 0.5f) * UI_FONT_CELL_WIDTH / target_width - 0.5f;
    float sy = ((float)ty + 0.5f) * UI_FONT_CELL_HEIGHT / target_height - 0.5f;
    sx = sx < 0.0f ? 0.0f : (sx > UI_FONT_CELL_WIDTH - 1 ? UI_FONT_CELL_WIDTH - 1 : sx);
    sy = sy < 0.0f ? 0.0f : (sy > UI_FONT_CELL_HEIGHT - 1 ? UI_FONT_CELL_HEIGHT - 1 : sy);

    const int x0 = (int)sx;
    const int y0 = (int)sy;
    const int x1 = min(x0 + 1, UI_FONT_CELL_WIDTH - 1);
    const int y1 = min(y0 + 1, UI_FONT_CELL_HEIGHT - 1);
    const float fx = sx - x0;
    const float fy = sy - y0;
    const float a00 = glyph[y0 * UI_FONT_CELL_WIDTH + x0];
    const float a10 = glyph[y0 * UI_FONT_CELL_WIDTH + x1];
    const float a01 = glyph[y1 * UI_FONT_CELL_WIDTH + x0];
    const float a11 = glyph[y1 * UI_FONT_CELL_WIDTH + x1];
    const float top = a00 + (a10 - a00) * fx;
    const float bottom = a01 + (a11 - a01) * fx;
    return (unsigned char)(top + (bottom - top) * fy + 0.5f);
}

// Draw one text plane with 1..4 interleaved channels. RGB/RGBA and the Y/UV
// planes of NV12/NV21 all use this path, so every OSD label has the same
// Cantarell metrics.
static void draw_text_plane(unsigned char* pixels, int w, int h, int stride,
                            int channels, const char* text, int x, int y,
                            int fontsize, unsigned int color)
{
    if (pixels == NULL || text == NULL || fontsize < 1) return;

    const unsigned char* pen_color = (const unsigned char*)&color;
    const int target_height = text_height(fontsize);
    const int target_width = max(1, (UI_FONT_CELL_WIDTH * target_height +
                                     UI_FONT_CELL_HEIGHT / 2) / UI_FONT_CELL_HEIGHT);
    int cursor_x = x;
    int cursor_y = y;

    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        if (*p == '\n') {
            cursor_x = x;
            cursor_y += target_height;
            continue;
        }
        if (*p < 32 || *p > 126) continue;

        const int index = (int)(*p - 32);
        const int x0 = max(cursor_x, 0);
        const int y0 = max(cursor_y, 0);
        const int x1 = min(cursor_x + target_width, w);
        const int y1 = min(cursor_y + target_height, h);
        for (int py = y0; py < y1; ++py) {
            unsigned char* row = pixels + (size_t)py * stride;
            for (int px = x0; px < x1; ++px) {
                const unsigned char alpha = sample_glyph(index, px - cursor_x,
                                                          py - cursor_y,
                                                          target_width, target_height);
                if (alpha == 0) continue;
                unsigned char* dst = row + (size_t)px * channels;
                const int ia = 255 - alpha;
                for (int channel = 0; channel < channels; ++channel) {
                    dst[channel] = (unsigned char)((dst[channel] * ia +
                                                   pen_color[channel] * alpha) / 255);
                }
            }
        }
        cursor_x += (ui_font_advance[index] * fontsize * 2 + UI_FONT_EM / 2) / UI_FONT_EM;
    }
}

static void draw_text_c1(unsigned char* pixels, int w, int h, const char* text, int x, int y,
                         int fontpixelsize, unsigned int color)
{
    draw_text_plane(pixels, w, h, w, 1, text, x, y, fontpixelsize, color);
}

static void draw_text_c2(unsigned char* pixels, int w, int h, const char* text, int x, int y,
                         int fontpixelsize, unsigned int color)
{
    draw_text_plane(pixels, w, h, w * 2, 2, text, x, y, fontpixelsize, color);
}

static void draw_text_c3(unsigned char* pixels, int w, int h, const char* text, int x, int y,
                         int fontpixelsize, unsigned int color)
{
    draw_text_plane(pixels, w, h, w * 3, 3, text, x, y, fontpixelsize, color);
}

static void draw_text_c4(unsigned char* pixels, int w, int h, const char* text, int x, int y,
                         int fontpixelsize, unsigned int color)
{
    draw_text_plane(pixels, w, h, w * 4, 4, text, x, y, fontpixelsize, color);
}

static void draw_text_yuv420sp(unsigned char* yuv420sp, int w, int h, const char* text,
                               int x, int y, int fontpixelsize, unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    unsigned char* Y = yuv420sp;
    draw_text_c1(Y, w, h, text, x, y, fontpixelsize, v_y);

    unsigned char* UV = yuv420sp + w * h;
    draw_text_c2(UV, w / 2, h / 2, text, x / 2, y / 2,
                 max(fontpixelsize / 2, 1), v_uv);
}

static void draw_image_c1(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + (y + i) * w + x,  draw_img + i * rw,  rw);
    }
}

static void draw_image_c2(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + ((y + i) * w + x) * 2,  draw_img + i * rw * 2,  rw * 2);
    }
}

static void draw_image_c3(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    printf("draw_image_c3 pixels=%p wxh=%dx%d draw_img=%p pos=(%d %d) rwxrh=%dx%d\n", pixels, w, h, draw_img, x, y, rw, rh);
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + ((y + i) * w + x) * 3,  draw_img + i * rw * 3,  rw * 3);
    }
}

static void draw_image_c4(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + ((y + i) * w + x) * 4,  draw_img + i * rw * 4,  rw * 4);
    }
}

static void draw_image_yuv420sp(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    draw_image_c1(pixels, w, h, draw_img, x, y, rw, rh);
    draw_image_c2(pixels, w, h / 2, draw_img + rw * rh, x, y/2, rw, rh/2);
}

void draw_rectangle(image_buffer_t* image, int rx, int ry, int rw, int rh, unsigned int color,
                      int thickness)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;

    unsigned int draw_color = convert_color(color, format);
    // printf("draw_color=%x\n", draw_color);

    // printf("draw_rectangle format=%d rx=%d ry=%d rw=%d rh=%d color=0x%x thickness=%d\n",
    //     format, rx, ry, rw, rh, color, thickness);
    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_rectangle_c3(pixels, w, h, rx, ry, rw, rh, draw_color, thickness);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_rectangle_c4(pixels, w, h, rx, ry, rw, rh, draw_color, thickness);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_rectangle_yuv420sp(pixels, w, h, rx, ry, rw, rh, draw_color, thickness);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

void draw_line(image_buffer_t* image, int x0, int y0, int x1, int y1, unsigned int color,
                 int thickness)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;

    unsigned draw_color = convert_color(color, format);

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_line_c3(pixels, w, h, x0, y0, x1, y1, draw_color, thickness);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_line_c4(pixels, w, h, x0, y0, x1, y1, draw_color, thickness);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_line_yuv420sp(pixels, w, h, x0, y0, x1, y1, draw_color, thickness);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

void rbbox_to_corners(const float *in_rbbox, int *out_rbbox) {
    // generate clockwise corners and rotate it clockwise
    // 顺时针方向返回角点位置
    float cx = in_rbbox[0] + in_rbbox[2] / 2;
    float cy = in_rbbox[1] + in_rbbox[3] / 2;
    float x_d = in_rbbox[2];
    float y_d = in_rbbox[3];
    float angle = in_rbbox[4];
    float a_cos = cos(angle);
    float a_sin = sin(angle);
    float corners_x[4] = {-x_d / 2, -x_d / 2, x_d / 2, x_d / 2};
    float corners_y[4] = {-y_d / 2, y_d / 2, y_d / 2, -y_d / 2};
    for (int i = 0; i < 4; ++i) {
        out_rbbox[2 * i] = (int)(a_cos * corners_x[i] - a_sin * corners_y[i] + cx);
        out_rbbox[2 * i + 1] = (int)(a_sin * corners_x[i] + a_cos * corners_y[i] + cy);
    }
}

void draw_obb_rectangle(image_buffer_t *image, int rx, int ry, int rw, int rh, float angle, unsigned int color,
                        int thickness) {
    float in_bbox[5] = {(float)(rx), (float)(ry), (float)(rw), (float)(rh), angle};
    int out_box_corners[8];
    rbbox_to_corners(in_bbox, out_box_corners);
    for(int i = 0 ; i < 4; i++) {
        int index1 = i;
        int index2 = (i + 1) % 4;
        draw_line(image, out_box_corners[index1 * 2], out_box_corners[index1 * 2 + 1],
                  out_box_corners[index2 * 2], out_box_corners[index2 * 2 + 1], color, thickness);
    }
}

void draw_text(image_buffer_t* image, const char* text, int x, int y, unsigned int color,
                 int fontsize)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;
    unsigned int draw_color = convert_color(color, format);

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_text_c3(pixels, w, h, text, x, y, fontsize, draw_color);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_text_c4(pixels, w, h, text, x, y, fontsize, draw_color);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_text_yuv420sp(pixels, w, h, text, x, y, fontsize, draw_color);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

void draw_circle(image_buffer_t* image, int cx, int cy, int radius, unsigned int color,
                 int thickness)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;
    unsigned draw_color = convert_color(color, format);

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_circle_c3(pixels, w, h, cx, cy, radius, draw_color, thickness);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_circle_c4(pixels, w, h, cx, cy, radius, draw_color, thickness);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_circle_yuv420sp(pixels, w, h, cx, cy, radius, draw_color, thickness);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

void draw_image(image_buffer_t* image, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_image_c3(pixels, w, h, draw_img, x, y, rw, rh);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_image_c4(pixels, w, h, draw_img, x, y, rw, rh);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_image_yuv420sp(pixels, w, h, draw_img, x, y, rw, rh);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}
