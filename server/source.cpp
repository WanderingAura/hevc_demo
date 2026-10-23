#include "server/source.hpp"

#include <cstdio>

namespace hs {

namespace {

constexpr uint8_t kBlack = 16;
constexpr uint8_t kWhite = 235;

// Bit i = segment i (a..g) lit.
constexpr uint8_t kDigitSegs[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

void fill_rect(const YuvView& v, int x, int y, int w, int h, uint8_t luma) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > v.width) w = v.width - x;
    if (y + h > v.height) h = v.height - y;
    for (int r = 0; r < h; ++r) std::memset(v.plane[0] + size_t(y + r) * v.stride[0] + x, luma, size_t(w > 0 ? w : 0));
}

int draw_text(const YuvView& v, const SevenSegLayout& L, int x, int y, const char* text) {
    for (const char* c = text; *c; ++c) {
        if (*c >= '0' && *c <= '9') {
            uint8_t segs = kDigitSegs[*c - '0'];
            for (int s = 0; s < 7; ++s) {
                if (!(segs >> s & 1)) continue;
                int rx, ry, rw, rh;
                segment_rect(L, x, y, s, rx, ry, rw, rh);
                fill_rect(v, rx, ry, rw, rh, kWhite);
            }
        } else if (*c == ':') {
            fill_rect(v, x, y + L.digit_h / 3 - L.thick / 2, L.thick, L.thick, kWhite);
            fill_rect(v, x, y + 2 * L.digit_h / 3 - L.thick / 2, L.thick, L.thick, kWhite);
        } else if (*c == '.') {
            fill_rect(v, x, y + L.digit_h - L.thick, L.thick, L.thick, kWhite);
        }
        x += glyph_advance(L, *c);
    }
    return x;
}

}  // namespace

SevenSegLayout seven_seg_layout(int width, int height) {
    SevenSegLayout L;
    L.digit_h = height / 12 > 14 ? height / 12 : 14;
    L.digit_w = L.digit_h / 2;
    L.thick = L.digit_h / 8 > 2 ? L.digit_h / 8 : 2;
    L.x0 = width / 40 + L.thick;
    L.y0 = height / 40 + L.thick;
    L.line_gap = L.digit_h + 3 * L.thick;
    return L;
}

int glyph_advance(const SevenSegLayout& L, char c) {
    return (c >= '0' && c <= '9') ? L.digit_w + 2 * L.thick : 3 * L.thick;
}

void segment_rect(const SevenSegLayout& L, int x, int y, int seg, int& rx, int& ry, int& rw, int& rh) {
    const int W = L.digit_w, H = L.digit_h, T = L.thick, half = H / 2;
    switch (seg) {
        case 0: rx = x, ry = y, rw = W, rh = T; break;                         // a: top
        case 1: rx = x + W - T, ry = y, rw = T, rh = half; break;              // b: top right
        case 2: rx = x + W - T, ry = y + half, rw = T, rh = H - half; break;   // c: bottom right
        case 3: rx = x, ry = y + H - T, rw = W, rh = T; break;                 // d: bottom
        case 4: rx = x, ry = y + half, rw = T, rh = H - half; break;           // e: bottom left
        case 5: rx = x, ry = y, rw = T, rh = half; break;                      // f: top left
        default: rx = x, ry = y + half - T / 2, rw = W, rh = T; break;         // g: middle
    }
}

void format_overlay(uint32_t index, int64_t capture_wall, char line1[9], char line2[13]) {
    std::snprintf(line1, 9, "%08u", index % 100000000u);
    int64_t ms_of_day = (capture_wall / kMs) % (24 * 3600 * 1000LL);
    if (ms_of_day < 0) ms_of_day += 24 * 3600 * 1000LL;
    unsigned h = unsigned(ms_of_day / 3600000), m = unsigned(ms_of_day / 60000 % 60),
             s = unsigned(ms_of_day / 1000 % 60), ms = unsigned(ms_of_day % 1000);
    std::snprintf(line2, 13, "%02u:%02u:%02u.%03u", h, m, s, ms);
}

void render_synthetic(const YuvView& v, uint32_t index, int64_t capture_wall) {
    // Moving diagonal gradient plus a sweeping bar, so there is real motion to encode.
    // Every row is the same 256-periodic ramp, shifted: one memcpy per row.
    uint8_t ramp[7680 + 256];
    for (int x = 0; x < v.width + 256; ++x) ramp[x] = uint8_t(kBlack + (x & 0xFF) * (kWhite - kBlack) / 255);
    const int bar_w = v.width / 24 > 2 ? v.width / 24 : 2;
    const int bar_x = int((uint64_t(index) * 12) % uint64_t(v.width));
    const int bar_end = bar_x + bar_w < v.width ? bar_x + bar_w : v.width;
    for (int y = 0; y < v.height; ++y) {
        uint8_t* row = v.plane[0] + size_t(y) * v.stride[0];
        std::memcpy(row, ramp + ((unsigned(y) + index * 3u) & 0xFF), size_t(v.width));
        std::memset(row + bar_x, kWhite, size_t(bar_end - bar_x));
    }
    const int cw = (v.width + 1) / 2, ch = (v.height + 1) / 2;
    for (int y = 0; y < ch; ++y) {
        uint8_t* u = v.plane[1] + size_t(y) * v.stride[1];
        uint8_t* w = v.plane[2] + size_t(y) * v.stride[2];
        for (int x = 0; x < cw; ++x) {
            u[x] = uint8_t(64 + x * 128 / cw);
            w[x] = uint8_t(64 + y * 128 / ch);
        }
    }

    char line1[9], line2[13];
    format_overlay(index, capture_wall, line1, line2);
    const SevenSegLayout L = seven_seg_layout(v.width, v.height);
    int text_w = 0;
    for (const char* c = line2; *c; ++c) text_w += glyph_advance(L, *c);
    const int bx = L.x0 - L.thick, by = L.y0 - L.thick, bw = text_w + L.thick, bh = L.line_gap + L.digit_h + 2 * L.thick;
    fill_rect(v, bx, by, bw, bh, kBlack);
    const int cx0 = bx / 2, cx1 = (bx + bw + 1) / 2 < cw ? (bx + bw + 1) / 2 : cw;
    for (int y = by / 2; y < (by + bh + 1) / 2 && y < ch; ++y) {  // neutral chroma: a grey box, not a tinted one
        std::memset(v.plane[1] + size_t(y) * v.stride[1] + cx0, 128, size_t(cx1 - cx0));
        std::memset(v.plane[2] + size_t(y) * v.stride[2] + cx0, 128, size_t(cx1 - cx0));
    }
    draw_text(v, L, L.x0, L.y0, line1);
    draw_text(v, L, L.x0, L.y0 + L.line_gap, line2);
}

}  // namespace hs
