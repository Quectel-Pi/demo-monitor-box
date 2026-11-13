#ifndef MONITOR_BOX_VIDEO_DIAGNOSTICS_OSD_H_
#define MONITOR_BOX_VIDEO_DIAGNOSTICS_OSD_H_

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "image_drawing.h"

namespace mb {

// Right-align Cantarell OSD text. Shrink the font when necessary so the
// complete string remains inside the image.
inline int draw_text_right(image_buffer_t* image, const char* text, int right, int y,
                           unsigned int color, int preferred_font) {
    if (image == nullptr || text == nullptr || text[0] == '\0') return 0;
    const int max_right = std::clamp(right, 0, image->width);
    int font = std::max(1, preferred_font);
    while (font > 1 && text_width(text, font) > max_right) --font;
    const int width = text_width(text, font);
    if (width <= 0 || width > max_right || y < 0 || y + text_height(font) > image->height)
        return 0;
    draw_text(image, text, max_right - width, y, color, font);
    return text_height(font);
}

// Bright, right-aligned status text with a black outline. The outline keeps
// the glyphs readable over both dark video and blown-out bright backgrounds.
inline int draw_text_right_highlighted(image_buffer_t* image, const char* text, int right, int y,
                                       unsigned int color, int preferred_font) {
    if (image == nullptr || text == nullptr || text[0] == '\0') return 0;
    const int max_right = std::clamp(right, 0, image->width);
    int font = std::max(1, preferred_font);
    while (font > 1 && text_width(text, font) > max_right) --font;
    const int width = text_width(text, font);
    if (width <= 0 || width > max_right || y < 0 || y + text_height(font) > image->height)
        return 0;
    const int x = max_right - width;
    draw_text(image, text, x + 1, y + 1, COLOR_BLACK, font);
    draw_text(image, text, x, y, color, font);
    return text_height(font);
}

inline int draw_diagnostics(image_buffer_t* image, long long frame, size_t detections,
                            size_t tracks, double ai_ms) {
    char text[192];
    snprintf(text, sizeof(text), "FRAME %lld  DET %zu  TRK %zu  AI %.1f ms",
             frame, detections, tracks, ai_ms);
    const int margin = std::min(8, image->width / 32);
    int font = std::clamp(image->width / 100, 4, 8);
    while (font > 1 && text_width(text, font) > image->width - 2 * margin) --font;
    if (font < 1 || text_width(text, font) > image->width - 2 * margin ||
        image->height < margin + text_height(font))
        return 0;
    draw_text(image, text, margin, margin, COLOR_WHITE, font);
    return margin + text_height(font);
}

}  // namespace mb

#endif  // MONITOR_BOX_VIDEO_DIAGNOSTICS_OSD_H_
