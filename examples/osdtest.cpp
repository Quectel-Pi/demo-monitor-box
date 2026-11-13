#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <vector>

#include "video/diagnostics_osd.h"
#include "video/region_overlay.h"
#include "video/alarm_osd.h"
#include "video/bev_osd.h"

int main() {
    for (const int width : {1, 64, 320, 640, 1920}) {
        for (const int height : {1, 24, 1080}) {
            for (const long long frame : {1LL, LLONG_MAX}) {
                const size_t size = (size_t)width * height * 3;
                std::vector<unsigned char> data(size + 64, 0x7F);
                image_buffer_t image = {};
                image.width = image.width_stride = width;
                image.height = image.height_stride = height;
                image.format = IMAGE_FORMAT_RGB888;
                image.virt_addr = data.data() + 32;
                image.size = (int)size;
                const int bottom = mb::draw_diagnostics(&image, frame, 123, 45, 123.4);
                assert(bottom >= 0 && bottom <= height);
                assert(bottom <= 24);
                assert(std::all_of(data.begin(), data.begin() + 32,
                                   [](unsigned char p) { return p == 0x7F; }));
                assert(std::all_of(data.end() - 32, data.end(),
                                   [](unsigned char p) { return p == 0x7F; }));
                for (size_t p = 32; p < size + 32; p += 3) {
                    // White text can only brighten the gray source, with no colored/dark fill.
                    assert(data[p] >= 0x7F);
                    assert(data[p] == data[p + 1] && data[p] == data[p + 2]);
                }
                if (width >= 320 && height >= 1080) {
                    assert(bottom > 0);
                    assert(std::any_of(data.begin() + 32, data.end() - 32,
                                       [](unsigned char p) { return p != 0x7F; }));
                    const size_t after = 32 + (size_t)bottom * width * 3;
                    assert(std::all_of(data.begin() + after, data.end() - 32,
                                       [](unsigned char p) { return p == 0x7F; }));
                }
            }
        }
    }
    puts("[PASS] compact white diagnostics: <=24px, no background, bounded writes at small/HD sizes");

    assert(text_height(12) == 24);
    assert(text_width("WWWW", 12) > text_width("iiii", 12));
    assert(text_width("gate-1  IN 3", 12) > text_width("gate-1 IN 3", 12));
    {
        const int width = 96;
        const int height = 32;
        std::vector<unsigned char> data((size_t)width * height * 3, 0);
        image_buffer_t image = {};
        image.width = image.width_stride = width;
        image.height = image.height_stride = height;
        image.format = IMAGE_FORMAT_RGB888;
        image.virt_addr = data.data();
        draw_text(&image, "ALARM  gate-1 IN", width + 40, height + 20,
                  0xFFFFFFFF, 14);
        draw_text(&image, "ALARM  gate-1 IN", -40, -20, 0xFFFFFFFF, 14);
    }
    puts("[PASS] Cantarell metrics are proportional and clipped OSD writes stay bounded");

    for (const int width : {1, 64, 320, 640, 1920}) {
        for (const int height : {1, 24, 64, 1080}) {
            const size_t size = (size_t)width * height * 3;
            std::vector<unsigned char> data(size + 64, 0x7F);
            image_buffer_t image = {};
            image.width = image.width_stride = width;
            image.height = image.height_stride = height;
            image.format = IMAGE_FORMAT_RGB888;
            image.virt_addr = data.data() + 32;
            image.size = (int)size;
            const int right = width - 2;
            const int y = height > 28 ? height - 26 : 0;
            const int drawn = mb::draw_text_right(&image, "2026-11-05 10:38:00", right, y,
                                                  COLOR_WHITE, 12);
            assert(drawn >= 0 && y + drawn <= height);
            assert(std::all_of(data.begin(), data.begin() + 32,
                               [](unsigned char p) { return p == 0x7F; }));
            assert(std::all_of(data.end() - 32, data.end(),
                               [](unsigned char p) { return p == 0x7F; }));
        }
    }
    puts("[PASS] right-aligned timestamp stays inside small and HD frames");

    {
        const int width = 320, height = 80;
        std::vector<unsigned char> data((size_t)width * height * 3, 0xFF);
        image_buffer_t image = {};
        image.width = image.width_stride = width;
        image.height = image.height_stride = height;
        image.format = IMAGE_FORMAT_RGB888;
        image.virt_addr = data.data();
        image.size = (int)data.size();
        const int drawn = mb::draw_text_right_highlighted(
            &image, "gate-1  IN 3  OUT 1  NOW 1", width - 6, 8, 0xFFFFD400, 12);
        assert(drawn > 0);
        bool yellow = false, outline = false;
        for (size_t p = 0; p < data.size(); p += 3) {
            yellow = yellow || (data[p] > 220 && data[p + 1] > 150 && data[p + 2] < 80);
            outline = outline || (data[p] < 40 && data[p + 1] < 40 && data[p + 2] < 40);
        }
        assert(yellow && outline);
    }
    puts("[PASS] highlighted right-aligned text uses amber glyphs with a black outline");

    // 区域填充: 只改多边形内部, 颜色按 alpha 混合, 越界顶点不写出图像
    {
        mb::ImageRGB img;
        img.width = 100;
        img.height = 50;
        img.data.assign(img.size_bytes(), 100);
        mb::OverlayLayer l;
        l.poly = {{10, 10}, {60, 10}, {60, 40}, {10, 40}};
        l.r = 200;
        l.g = 0;
        l.b = 100;
        l.alpha = 128;
        mb::OverlayLayer outside;
        outside.poly = {{-50, -50}, {500, -50}, {500, 5}, {-50, 5}};
        outside.r = outside.g = outside.b = 0;
        outside.alpha = 256;
        mb::fill_regions(&img, {l, outside});
        const uint8_t* in = img.row(20) + 30 * 3;
        assert(in[0] == 150 && in[1] == 50 && in[2] == 100);
        const uint8_t* out = img.row(20) + 70 * 3;
        assert(out[0] == 100 && out[1] == 100 && out[2] == 100);
        assert(img.row(10)[10 * 3] == 150 && img.row(9)[10 * 3] == 100);
        assert(img.row(39)[59 * 3] == 150 && img.row(40)[59 * 3] == 100 && img.row(39)[60 * 3] == 100);
        assert(img.row(0)[0] == 0 && img.row(4)[99 * 3] == 0 && img.row(5)[0] == 100);
        size_t changed = 0;
        for (size_t i = 0; i < img.data.size(); i += 3) changed += img.data[i] != 100;
        assert(changed == 50 * 30 + 100 * 5);

        // 凹多边形 (U 形) 的缺口不填
        img.data.assign(img.size_bytes(), 100);
        l.poly = {{0, 0}, {30, 0}, {30, 40}, {60, 40}, {60, 0}, {90, 0}, {90, 50}, {0, 50}};
        mb::fill_regions(&img, {l});
        assert(img.row(20)[45 * 3] == 100 && img.row(20)[15 * 3] == 150 && img.row(45)[45 * 3] == 150);
    }
    puts("[PASS] region fill: spans, alpha blend, clipping, concave polygon");

    {
        mb::AlarmHighlights highlights;
        mb::RuleEvent intrusion;
        intrusion.feature = "intrusion";
        intrusion.kind = "enter";
        intrusion.rule_id = "yard";
        intrusion.track_id = 7;
        mb::RuleEvent package = intrusion;
        package.feature = "package";
        package.track_id = 9;
        highlights.update({intrusion, package}, 1000);
        assert(highlights.track(7) && highlights.track(9));
        assert(!highlights.track(8) && !highlights.track(-1));
        assert(!highlights.region("yard"));
        highlights.update({}, 3999);
        assert(highlights.track(7));
        highlights.update({}, 4000);
        assert(!highlights.track(7) && !highlights.track(9));
        highlights.update({intrusion}, 5000);
        for (uint64_t ts = 6000; ts <= 65000; ts += 1000) {
            highlights.update({intrusion}, ts);  // 录像续时, 不能只高亮触发的一帧
            assert(highlights.track(7));
        }
        highlights.update({}, 67999);
        assert(highlights.track(7));
        highlights.update({}, 68000);
        assert(!highlights.track(7));
        intrusion.track_id = -1;
        highlights.update({intrusion}, 70000);
        assert(highlights.region("yard") && !highlights.region("street"));
        highlights.clear();
        assert(!highlights.region("yard"));

        mb::RuleEvent fire;
        fire.feature = "fire";
        fire.cls_id = 0;
        fire.x = fire.y = 50;
        mb::Detection other, source, smoke;
        other.cls_id = source.cls_id = 0;
        smoke.cls_id = 1;
        other.x1 = other.y1 = 40;
        other.x2 = other.y2 = 100;
        source.x1 = source.y1 = smoke.x1 = smoke.y1 = 30;
        source.x2 = source.y2 = smoke.x2 = smoke.y2 = 70;
        const std::vector<mb::Detection> visible{other, source, smoke};
        highlights.update({fire}, 80000);
        assert(!highlights.track(0));
        assert(!highlights.fire(0, visible) && highlights.fire(1, visible));
        assert(!highlights.fire(2, visible) && !highlights.fire(3, visible));
        assert(!highlights.fire(0, {}) && !highlights.fire(0, {smoke}));
        highlights.clear();
        assert(!highlights.fire(1, visible));
    }
    puts("[PASS] alarm sources: exact track, expiry, extended clips, region, fire class/location");

    for (const int height : {240, 720, 1080}) {
        const int width = height == 1080 ? 1920 : 320;
        for (const unsigned char background : {0, 127, 255}) {
            std::vector<unsigned char> data((size_t)width * height * 3, background);
            image_buffer_t image = {};
            image.width = image.width_stride = width;
            image.height = image.height_stride = height;
            image.format = IMAGE_FORMAT_RGB888;
            image.virt_addr = data.data();
            mb::draw_alarm_box(&image, 40, 40, 200, 140);
            const int thickness = mb::alarm_thickness(&image);
            assert(thickness == (height == 1080 ? 6 : 4));
            auto pixel = [&](int x, int y) { return data.data() + ((size_t)y * width + x) * 3; };
            for (int x = 40 - thickness / 2; x < 40 + thickness - thickness / 2; ++x) {
                const auto p = pixel(x, 110);
                assert(p[0] == 255 && p[1] == 0 && p[2] == 0);
            }
            const auto white = pixel(40 - thickness / 2 - 1, 110);
            const auto black = pixel(40 - thickness / 2 - 3, 110);
            assert(white[0] == 255 && white[1] == 255 && white[2] == 255);
            assert(black[0] == 0 && black[1] == 0 && black[2] == 0);
            const auto inside = pixel(140, 110);
            assert(inside[0] == background && inside[1] == background && inside[2] == background);
            const auto outside = pixel(10, 110);
            assert(outside[0] == background && outside[1] == background && outside[2] == background);
            const int inset = (thickness + 8) / 2 + 1;
            for (int y = 40 + inset; y < 180 - inset; ++y)
                for (int x = 40 + inset; x < 240 - inset; ++x) {
                    const auto p = pixel(x, y);
                    assert(p[0] == background && p[1] == background && p[2] == background);
                }
        }
    }
    for (const int width : {1, 4, 32, 320, 1920}) {
        for (const int height : {1, 4, 24, 1080}) {
            const size_t size = (size_t)width * height * 3;
            for (const int offset : {-20, 0, 20}) {
                std::vector<unsigned char> data(size + 64, 0x7F);
                image_buffer_t image = {};
                image.width = image.width_stride = width;
                image.height = image.height_stride = height;
                image.format = IMAGE_FORMAT_RGB888;
                image.virt_addr = data.data() + 32;
                mb::draw_alarm_box(&image, offset, offset, width, height);
                assert(std::all_of(data.begin(), data.begin() + 32,
                                   [](unsigned char p) { return p == 0x7F; }));
                assert(std::all_of(data.end() - 32, data.end(),
                                   [](unsigned char p) { return p == 0x7F; }));
            }
        }
    }
    puts("[PASS] alarm drawing: 6px at HD, black/white contrast, no label or interior fill, clipped writes");

    {
        mb::BevScene s;
        s.valid = true;
        s.hull_m = {{0, 0}, {6, 0}, {6, 10}, {0, 10}};
        mb::BevScene::Zone z;
        z.pts = {{1, 1}, {3, 1}, {3, 4}, {1, 4}};
        z.name = "door";
        z.occupied = true;
        s.zones.push_back(z);
        mb::BevScene::Target t;
        t.id = 7;
        t.group = "person";
        t.pos = {2, 2};
        t.has_heading = true;
        t.heading_deg = 90;
        t.trail = {{2, 0.5f}, {2, 1.2f}, {2, 2}};
        s.targets.push_back(t);
        mb::BevScene::Target far = t;  // 远离标定区的离群点: 不能把视野撑爆, 也不能画出框外
        far.id = 8;
        far.pos = {500, -300};
        far.trail = {{2, 2}, {500, -300}};
        s.targets.push_back(far);

        mb::BevPipParams p;
        assert(p.size == 0.2f);
        mb::ImageRGB img;
        img.width = 1280;
        img.height = 720;
        img.data.assign(img.size_bytes(), 200);
        assert(!mb::draw_bev_pip(&img, s, p));  // 默认关闭
        assert(std::all_of(img.data.begin(), img.data.end(), [](uint8_t v) { return v == 200; }));

        for (const char* corner : {"top_left", "top_right", "bottom_left", "bottom_right"}) {
            img.data.assign(img.size_bytes(), 200);
            p.enabled = true;
            p.corner = corner;
            mb::BevView v;
            assert(mb::draw_bev_pip(&img, s, p, &v));
            assert(v.x >= 0 && v.y >= 0 && v.x + v.w <= img.width && v.y + v.h <= img.height);
            assert(v.w >= (int)(0.19f * img.width) || v.h >= img.height * 6 / 10 - 1);
            assert(corner[0] == 't' ? v.y == 4 : v.y + v.h == img.height - 4);
            const bool right_edge = std::string(corner).find("right") != std::string::npos;
            assert(right_edge ? v.x + v.w == img.width - 4 : v.x == 4);
            // 世界范围由标定区决定, 离群点不参与
            assert(v.maxx - v.minx < 20.0f && v.maxy - v.miny < 20.0f);
            // 画中画外一个像素都不能动
            size_t changed_out = 0, changed_in = 0;
            for (int y = 0; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x) {
                    const uint8_t* px = img.row(y) + x * 3;
                    const bool in = x >= v.x && x < v.x + v.w && y >= v.y && y < v.y + v.h;
                    const bool diff = px[0] != 200 || px[1] != 200 || px[2] != 200;
                    if (diff) ++(in ? changed_in : changed_out);
                }
            assert(changed_out == 0);
            assert(changed_in > (size_t)(v.w * v.h) / 2);  // 背景压暗
            // 目标点画在 (2,2) 处, 是人员组的绿色
            const mb::Point c = v.to_px({2, 2});
            const uint8_t* px = img.row((int)c.y) + (int)c.x * 3;
            assert(px[1] > px[0] && px[1] > px[2]);
            // y 轴向上: 远处 (y 大) 在画中画上方
            assert(v.to_px({0, 10}).y < v.to_px({0, 0}).y);
        }
        p.corner = "bottom_right";
        mb::BevView v;
        assert(mb::bev_layout(s, p, 1280, 720, &v));
        assert(v.x + v.w == img.width - 4 && v.y + v.h == img.height - 4);

        mb::BevScene empty;
        assert(!mb::draw_bev_pip(&img, empty, p));
        img.width = 100;  // 画面太小不画
        img.height = 80;
        img.data.assign(img.size_bytes(), 0);
        assert(!mb::draw_bev_pip(&img, s, p));

        mb::Json cfg;
        std::string err;
        mb::Json::parse(R"({"display": {"bev_pip": {"enabled": true, "corner": "nowhere", "size": 2}}})",
                        &cfg, &err);
        const mb::BevPipParams q = mb::BevPipParams::from_json(cfg);
        assert(q.enabled && q.corner == "bottom_left" && q.size == 0.5f);
        assert(!mb::BevPipParams::from_json(mb::Json::object()).enabled);
    }
    puts("[PASS] bev pip: off by default, four corners, clipped to its box, outliers ignored, y-up");
}
