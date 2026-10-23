// Synthetic camera input: a moving pattern plus a burned-in 7-segment frame
// counter and capture time, so every frame can be verified by eye.
#pragma once
#include "common/ports.hpp"

namespace hs {

struct SevenSegLayout {
    int x0, y0;           // top-left of the first glyph of line 1
    int digit_w, digit_h;
    int thick;
    int line_gap;         // y distance between the two text lines
};

SevenSegLayout seven_seg_layout(int width, int height);
int glyph_advance(const SevenSegLayout& L, char c);

// Segment `seg` (0..6 = a..g) of a digit whose top-left is (x, y).
void segment_rect(const SevenSegLayout& L, int x, int y, int seg, int& rx, int& ry, int& rw, int& rh);

// Line 1: 8-digit frame index. Line 2: "HH:MM:SS.mmm" UTC of capture_wall.
void format_overlay(uint32_t index, int64_t capture_wall, char line1[9], char line2[13]);

void render_synthetic(const YuvView& v, uint32_t index, int64_t capture_wall);

struct SyntheticSource final : FrameSource {
    void fill(const YuvView& dst, uint32_t index, int64_t capture_wall) override {
        render_synthetic(dst, index, capture_wall);
    }
};

}  // namespace hs
