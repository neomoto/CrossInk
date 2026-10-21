#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "lib/GfxRenderer/BitmapRow.h"

namespace {

// A non-byte-aligned panel also makes the unused bits in each destination
// row observable. They must survive both horizontal writes and vertical steps.
constexpr int PANEL_WIDTH = 43;
constexpr int PANEL_HEIGHT = 39;
constexpr int STRIDE = (PANEL_WIDTH + 7) / 8;
constexpr std::array<bitmapRow::Plane, 5> PLANES = {
    bitmapRow::Plane::BW, bitmapRow::Plane::OverlayLSB, bitmapRow::Plane::OverlayMSB,
    bitmapRow::Plane::AbsoluteLSB, bitmapRow::Plane::AbsoluteMSB};

struct Rect {
  int left;
  int top;
  int right;
  int bottom;
};

std::pair<int, int> physical(int orientation, int x, int y) {
  switch (orientation) {
    case 0:
      return {y, PANEL_HEIGHT - 1 - x};
    case 1:
      return {PANEL_WIDTH - 1 - x, PANEL_HEIGHT - 1 - y};
    case 2:
      return {PANEL_WIDTH - 1 - y, x};
    default:
      return {x, y};
  }
}

std::pair<int, int> logical(int orientation, int x, int y) {
  switch (orientation) {
    case 0:
      return {PANEL_HEIGHT - 1 - y, x};
    case 1:
      return {PANEL_WIDTH - 1 - x, PANEL_HEIGHT - 1 - y};
    case 2:
      return {y, PANEL_WIDTH - 1 - x};
    default:
      return {x, y};
  }
}

glyphBitmap::Frame frameFor(int orientation, int x, int y) {
  switch (orientation) {
    case 0:
      return {y, PANEL_HEIGHT - 1 - x, 0, -1, 1, 0};
    case 1:
      return {PANEL_WIDTH - 1 - x, PANEL_HEIGHT - 1 - y, -1, 0, 0, -1};
    case 2:
      return {PANEL_WIDTH - 1 - y, x, 0, 1, -1, 0};
    default:
      return {x, y, 1, 0, 0, 1};
  }
}

// Intentionally scalar and independent of the rasterizer's bit-compaction
// formulas, masks, clipping helpers and framebuffer-address walking.
void paintReference(const uint8_t* row, int width, int left, int right, bitmapRow::Plane plane, int orientation,
                    int x, int y, int originY, int rows, int guard, std::vector<uint8_t>& expected) {
  for (int source = 0; source < width; ++source) {
    if (source < left || source >= right) continue;
    const auto [px, py] = physical(orientation, x + source, y);
    if (px < 0 || px >= PANEL_WIDTH || py < originY || py >= originY + rows) continue;
    const int value = (row[source / 4] >> (6 - 2 * (source % 4))) & 3;
    bool write = true;
    bool black = false;
    switch (plane) {
      case bitmapRow::Plane::BW:
        write = value != 3;
        black = true;
        break;
      case bitmapRow::Plane::OverlayLSB:
        write = value == 1;
        break;
      case bitmapRow::Plane::OverlayMSB:
        write = value == 1 || value == 2;
        break;
      case bitmapRow::Plane::AbsoluteLSB:
        black = value == 0 || value == 2;
        break;
      case bitmapRow::Plane::AbsoluteMSB:
        black = value == 0 || value == 1;
        break;
    }
    if (!write) continue;
    auto& byte = expected[guard + (py - originY) * STRIDE + px / 8];
    const uint8_t bit = static_cast<uint8_t>(1u << (7 - px % 8));
    if (black)
      byte &= static_cast<uint8_t>(~bit);
    else
      byte |= bit;
  }
}

std::vector<uint8_t> background(int rows, int guard) {
  std::vector<uint8_t> bytes(rows * STRIDE + 2 * guard);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(0xa5u + i * 53u);
  return bytes;
}

std::vector<uint8_t> packedRow(int width, uint8_t seed, int padding = 0) {
  std::vector<uint8_t> row((width + 3) / 4 + padding);
  for (size_t i = 0; i < row.size(); ++i) row[i] = static_cast<uint8_t>(seed + i * 73u);
  return row;
}

void compareRow(const std::vector<uint8_t>& row, int width, int left, int right, bitmapRow::Plane plane,
                int orientation, int x, int y, int originY = 0, int rows = PANEL_HEIGHT, int guard = 8) {
  SCOPED_TRACE(::testing::Message() << "plane=" << static_cast<int>(plane) << " orientation=" << orientation
                                  << " width=" << width << " source=" << left << ',' << right << " at=" << x << ','
                                  << y << " band=" << originY << ',' << rows << " guard=" << guard);
  auto expected = background(rows, guard);
  auto actual = expected;
  paintReference(row.data(), width, left, right, plane, orientation, x, y, originY, rows, guard, expected);

  // Exact allocations make source overreads and even zero-mask accesses just
  // beyond the destination visible under ASan, rather than hidden by capacity.
  const auto source = std::make_unique<uint8_t[]>(row.size());
  const auto destination = std::make_unique<uint8_t[]>(actual.size());
  std::copy(row.begin(), row.end(), source.get());
  std::copy(actual.begin(), actual.end(), destination.get());
  const glyphBitmap::Target target{destination.get() + guard, PANEL_WIDTH, STRIDE, originY, rows,
                                   frameFor(orientation, x, y)};
  bitmapRow::draw(source.get(), width, left, right, plane, target);
  std::copy_n(destination.get(), actual.size(), actual.begin());
  EXPECT_EQ(expected, actual);
}

void comparePhysical(const std::vector<uint8_t>& row, int width, int left, int right, bitmapRow::Plane plane,
                     int orientation, int px, int py, int originY = 0, int rows = PANEL_HEIGHT, int guard = 8) {
  const auto [x, y] = logical(orientation, px, py);
  compareRow(row, width, left, right, plane, orientation, x, y, originY, rows, guard);
}

}  // namespace

TEST(BitmapRow, EveryPackedByteAcrossPlanesRotationsAndBitAlignments) {
  for (int packed = 0; packed < 256; ++packed) {
    const std::vector<uint8_t> row{static_cast<uint8_t>(packed), static_cast<uint8_t>(packed ^ 0xff),
                                   static_cast<uint8_t>((packed << 4) | (packed >> 4))};
    for (const auto plane : PLANES) {
      for (int orientation = 0; orientation < 4; ++orientation) {
        for (int alignment = 0; alignment < 8; ++alignment) {
          comparePhysical(row, 12, 0, 12, plane, orientation, 16 + alignment, 20);
        }
      }
    }
  }
}

TEST(BitmapRow, CroppedPrefixesOddTailsAndSourcePadding) {
  for (const auto plane : PLANES) {
    for (int orientation = 0; orientation < 4; ++orientation) {
      for (int prefix = 0; prefix < 4; ++prefix) {
        for (int kept = 1; kept <= 17; ++kept) {
          const int width = prefix + kept;
          for (int alignment = 0; alignment < 8; ++alignment) {
            const uint8_t seed = static_cast<uint8_t>(width * 17 + prefix * 29 + alignment);
            comparePhysical(packedRow(width, seed), width, prefix, width, plane, orientation, 16 + alignment, 20);
            comparePhysical(packedRow(width + 3, seed, 4), width + 3, prefix, width, plane, orientation,
                            16 + alignment, 20);
          }
        }
      }
    }
  }
}

TEST(BitmapRow, NegativeOffsetsPanelClippingAndPhysicalStripBands) {
  constexpr std::array<std::pair<int, int>, 5> BANDS = {
      std::pair{0, PANEL_HEIGHT}, std::pair{0, 1}, std::pair{7, 1}, std::pair{8, 4},
      std::pair{PANEL_HEIGHT - 2, 2}};
  for (const auto plane : PLANES) {
    for (int orientation = 0; orientation < 4; ++orientation) {
      const int screenWidth = orientation % 2 == 0 ? PANEL_HEIGHT : PANEL_WIDTH;
      const int screenHeight = orientation % 2 == 0 ? PANEL_WIDTH : PANEL_HEIGHT;
      for (int width : {1, 2, 3, 7, 8, 9, 17, 41, 53}) {
        const auto row = packedRow(width, static_cast<uint8_t>(width * 11));
        for (const auto [x, y] : {std::pair{-9, 0}, std::pair{-3, 7}, std::pair{3, -1}, std::pair{4, 9},
                                  std::pair{screenWidth - 3, screenHeight - 1}, std::pair{screenWidth, 4},
                                  std::pair{1, screenHeight}}) {
          for (const auto [origin, rows] : BANDS) {
            for (const auto [left, right] : {std::pair{0, width}, std::pair{1, width - 1}, std::pair{3, width - 2},
                                             std::pair{-2, width + 3}, std::pair{5, 2}}) {
              compareRow(row, width, left, right, plane, orientation, x, y, origin, rows);
            }
          }
        }
      }
    }
  }
}

TEST(BitmapRow, ExactBufferEdgesPreserveLastBytePadding) {
  for (const auto plane : PLANES) {
    for (int orientation = 0; orientation < 4; ++orientation) {
      for (int prefix = 0; prefix < 4; ++prefix) {
        for (int kept = 1; kept <= 17; ++kept) {
          const int width = prefix + kept;
          for (bool atEnd : {false, true}) {
            const int horizontalStep = orientation == 1 ? -1 : orientation == 3 ? 1 : 0;
            const int verticalStep = orientation == 0 ? -1 : orientation == 2 ? 1 : 0;
            const int left = atEnd ? PANEL_WIDTH - kept : 0;
            const int top = atEnd ? PANEL_HEIGHT - kept : 0;
            const int px = horizontalStep != 0 ? left + (horizontalStep < 0 ? kept - 1 : 0) - prefix * horizontalStep
                                               : (atEnd ? PANEL_WIDTH - 1 : 0);
            const int py = verticalStep != 0 ? top + (verticalStep < 0 ? kept - 1 : 0) - prefix * verticalStep
                                             : (atEnd ? PANEL_HEIGHT - 1 : 0);
            const int origin = verticalStep != 0 ? top : py;
            const int rows = verticalStep != 0 ? kept : 1;
            for (uint8_t seed : {0x00, 0x1b, 0x55, 0xaa, 0xe4, 0xff}) {
              comparePhysical(packedRow(width, seed), width, prefix, width, plane, orientation, px, py, origin, rows, 0);
            }
          }
        }
      }
    }
  }
}

TEST(BitmapRow, SequentialBytePaddedRowsWithLogicalClipMatchScalarImage) {
  constexpr int WIDTH = 17;
  constexpr int HEIGHT = 13;
  constexpr int ROW_BYTES = (WIDTH + 3) / 4 + 3;
  constexpr int CROP_X = 3;
  constexpr int CROP_Y = 2;
  constexpr int X = -2;
  constexpr int Y = 6;
  constexpr Rect CLIP{1, 7, 12, 16};
  std::array<uint8_t, ROW_BYTES * HEIGHT> image{};
  for (size_t i = 0; i < image.size(); ++i) image[i] = static_cast<uint8_t>(0xe4u + i * 73u);
  for (const auto plane : PLANES) {
    for (int orientation = 0; orientation < 4; ++orientation) {
      for (bool topDown : {false, true}) {
        auto expected = background(PANEL_HEIGHT, 0);
        auto actual = expected;
        for (int bmpY = 0; bmpY < HEIGHT - CROP_Y; ++bmpY) {
          if (bmpY < CROP_Y) continue;
          const int screenY = Y - CROP_Y + (topDown ? bmpY : HEIGHT - 1 - bmpY);
          if (screenY < CLIP.top || screenY >= CLIP.bottom) continue;
          const int rowX = X - CROP_X;
          const int left = std::max(CROP_X, CLIP.left - rowX);
          const int right = std::min(WIDTH - CROP_X, CLIP.right - rowX);
          const uint8_t* row = image.data() + bmpY * ROW_BYTES;
          paintReference(row, WIDTH, left, right, plane, orientation, rowX, screenY, 0, PANEL_HEIGHT, 0, expected);
          const glyphBitmap::Target target{actual.data(), PANEL_WIDTH, STRIDE, 0, PANEL_HEIGHT,
                                           frameFor(orientation, rowX, screenY)};
          bitmapRow::draw(row, WIDTH, left, right, plane, target);
        }
        EXPECT_EQ(expected, actual) << "plane=" << static_cast<int>(plane) << " orientation=" << orientation
                                    << " topDown=" << topDown;
      }
    }
  }
}
