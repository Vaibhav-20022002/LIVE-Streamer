#include "inference/yuv_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace streamer {

/// 5x7 bitmap font covering A-Z, 0-9, '.', '-', ':', '%', and space (5 bits per
/// row).
struct FontChar {
  char c;
  uint8_t rows[7];
};

static const FontChar kFont5x7[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0E}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x13, 0x15, 0x19, 0x11, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x01, 0x02, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}};

static const uint8_t *get_font_char(char c) {
  if (c >= 'a' && c <= 'z') {
    c = c - 'a' + 'A';
  }
  for (const auto &fc : kFont5x7) {
    if (fc.c == c) {
      return fc.rows;
    }
  }
  /// Fallback to space
  for (const auto &fc : kFont5x7) {
    if (fc.c == ' ') {
      return fc.rows;
    }
  }
  return kFont5x7[0].rows;
}

/// Color palette per class_id % kPaletteSize in YUV (Y, U, V) tuples
struct YUVColor {
  uint8_t y;
  uint8_t u;
  uint8_t v;
};

static constexpr int kPaletteSize = 6;
static const YUVColor kPalette[kPaletteSize] = {
    {145, 54, 34},   //< Bright Green (YUV approx)
    {162, 44, 142},  //< Cyan
    {226, 30, 85},   //< Yellow
    {107, 133, 202}, //< Orange / Red-ish
    {112, 179, 117}, //< Magenta
    {210, 128, 128}  //< White / Light Gray
};

void draw_detections_yuv420p(uint8_t *y_plane, int y_stride, uint8_t *u_plane,
                             int u_stride, uint8_t *v_plane, int v_stride,
                             int width, int height,
                             const std::vector<Detection> &detections) {
  if (!y_plane || !u_plane || !v_plane || width <= 0 || height <= 0 ||
      detections.empty()) {
    return;
  }

  auto set_pixel_yuv = [&](int x, int y, uint8_t Y, uint8_t U, uint8_t V) {
    if (x < 0 || x >= width || y < 0 || y >= height) {
      return;
    }
    y_plane[y * y_stride + x] = Y;
    u_plane[(y / 2) * u_stride + (x / 2)] = U;
    v_plane[(y / 2) * v_stride + (x / 2)] = V;
  };

  for (const auto &det : detections) {
    int x1 = static_cast<int>(std::round(det.x1));
    int y1 = static_cast<int>(std::round(det.y1));
    int x2 = static_cast<int>(std::round(det.x2));
    int y2 = static_cast<int>(std::round(det.y2));

    /// Clamp box coords to [0, width - 1] / [0, height - 1]
    x1 = std::clamp(x1, 0, width - 1);
    y1 = std::clamp(y1, 0, height - 1);
    x2 = std::clamp(x2, 0, width - 1);
    y2 = std::clamp(y2, 0, height - 1);

    if (x2 <= x1 || y2 <= y1) {
      continue;
    }

    YUVColor col = kPalette[std::abs(det.class_id) % kPaletteSize];

    /// Draw 2px thick rectangle border
    /// Top and bottom borders (rows y1, y1+1, y2-1, y2)
    for (int t = 0; t < 2; ++t) {
      int cur_y1 = y1 + t;
      int cur_y2 = y2 - t;
      if (cur_y1 < height) {
        for (int x = x1; x <= x2; ++x) {
          set_pixel_yuv(x, cur_y1, col.y, col.u, col.v);
        }
      }
      if (cur_y2 >= 0 && cur_y2 != cur_y1) {
        for (int x = x1; x <= x2; ++x) {
          set_pixel_yuv(x, cur_y2, col.y, col.u, col.v);
        }
      }
    }
    /// Left and right borders (cols x1, x1+1, x2-1, x2)
    for (int t = 0; t < 2; ++t) {
      int cur_x1 = x1 + t;
      int cur_x2 = x2 - t;
      if (cur_x1 < width) {
        for (int y = y1; y <= y2; ++y) {
          set_pixel_yuv(cur_x1, y, col.y, col.u, col.v);
        }
      }
      if (cur_x2 >= 0 && cur_x2 != cur_x1) {
        for (int y = y1; y <= y2; ++y) {
          set_pixel_yuv(cur_x2, y, col.y, col.u, col.v);
        }
      }
    }

    /// Prepare label text e.g. "PERSON 0.91"
    char buf[64];
    std::string upper_label = det.label;
    for (auto &ch : upper_label) {
      ch = std::toupper(static_cast<unsigned char>(ch));
    }
    std::snprintf(buf, sizeof(buf), "%s %.2f", upper_label.c_str(),
                  det.confidence);
    std::string text(buf);

    /// Header strip height = 12px, positioned above box or inside top of box if
    /// y1 < 12
    int strip_h = 12;
    int strip_y1 = (y1 >= strip_h) ? (y1 - strip_h) : y1;
    int strip_y2 = strip_y1 + strip_h - 1;

    /// Calculate text width: each char is 5 pixels wide + 1 pixel spacing = 6
    /// pixels per char
    int text_w = static_cast<int>(text.size()) * 6 - 1;
    int strip_x1 = x1;
    int strip_x2 = std::min(width - 1, x1 + text_w + 4);

    /// Draw filled dark/colored header strip
    for (int sy = strip_y1; sy <= strip_y2; ++sy) {
      for (int sx = strip_x1; sx <= strip_x2; ++sx) {
        /// Dark background for header strip (Y=32, U=128, V=128) or box color
        set_pixel_yuv(sx, sy, 32, 128, 128);
      }
    }

    /// Render text at 1x scale inside header strip
    int cursor_x = strip_x1 + 2;
    int cursor_y =
        strip_y1 + (strip_h - 7) / 2; //< vertically centered (7px font height)

    for (char c : text) {
      const uint8_t *glyph = get_font_char(c);
      for (int row = 0; row < 7; ++row) {
        uint8_t row_bits = glyph[row];
        for (int col = 0; col < 5; ++col) {
          /// Bit 4 is leftmost (MSB of 5 bits: 0x10, 0x08, 0x04, 0x02, 0x01)
          if (row_bits & (1 << (4 - col))) {
            set_pixel_yuv(cursor_x + col, cursor_y + row, 235, 128,
                          128); //< Bright white text
          }
        }
      }
      cursor_x += 6; //< 5 px font + 1 px spacing
    }
  }
}

} //< namespace streamer
