#include <gtest/gtest.h>

#include "server/source.hpp"

using namespace hs;

namespace {

constexpr uint8_t kDigitSegs[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

// Reads a rendered string back by sampling each segment's centre.
std::string read_line(const YuvView& v, const SevenSegLayout& L, int y, const char* layout) {
    std::string out;
    int x = L.x0;
    for (const char* c = layout; *c; ++c) {
        if (*c == '0') {
            uint8_t mask = 0;
            for (int s = 0; s < 7; ++s) {
                int rx, ry, rw, rh;
                segment_rect(L, x, y, s, rx, ry, rw, rh);
                if (v.plane[0][size_t(ry + rh / 2) * v.stride[0] + rx + rw / 2] == 235) mask |= 1 << s;
            }
            char d = '?';
            for (int i = 0; i < 10; ++i)
                if (kDigitSegs[i] == mask) d = char('0' + i);
            out += d;
        } else {
            out += *c;
        }
        x += glyph_advance(L, *c == '0' ? '0' : *c);
    }
    return out;
}

}  // namespace

TEST(Source, OverlayReadsBack) {
    const int w = 640, h = 360;
    std::vector<uint8_t> y(size_t(w) * h), u(size_t(w / 2) * (h / 2)), v(size_t(w / 2) * (h / 2));
    YuvView view{{y.data(), u.data(), v.data()}, {w, w / 2, w / 2}, w, h};
    // 12:34:56.789 UTC on some day
    int64_t wall = (int64_t(19000) * 86400 + 12 * 3600 + 34 * 60 + 56) * kSec + 789 * kMs;
    render_synthetic(view, 1234567, wall);

    SevenSegLayout L = seven_seg_layout(w, h);
    EXPECT_EQ(read_line(view, L, L.y0, "00000000"), "01234567");
    EXPECT_EQ(read_line(view, L, L.y0 + L.line_gap, "00:00:00.000"), "12:34:56.789");
}

TEST(Source, PatternMovesBetweenFrames) {
    const int w = 320, h = 180;
    std::vector<uint8_t> a(size_t(w) * h * 3 / 2), b(a.size());
    auto view = [&](std::vector<uint8_t>& buf) {
        return YuvView{{buf.data(), buf.data() + w * h, buf.data() + w * h * 5 / 4}, {w, w / 2, w / 2}, w, h};
    };
    render_synthetic(view(a), 1, 0);
    render_synthetic(view(b), 2, 0);
    EXPECT_NE(a, b);
    std::vector<uint8_t> c(a.size());
    render_synthetic(view(c), 1, 0);
    EXPECT_EQ(a, c);  // deterministic
}
