#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "lib/GfxRenderer/GlyphBitmap.h"

namespace {
constexpr int PANEL_WIDTH = 40;
constexpr int PANEL_HEIGHT = 32;
constexpr int STRIDE = PANEL_WIDTH / 8;

using glyphBitmap::Plane;

// Logical placements used by the renderer: unrotated text, and side-button
// labels rotated 90 degrees clockwise (glyph x runs up, glyph y runs right).
constexpr std::array<std::array<int, 4>, 2> AXES = {{{1, 0, 0, 1}, {0, -1, 1, 0}}};

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

// Logical rectangle the renderer clips to; the glyph-local clip is derived from it.
struct Rect {
  int left;
  int top;
  int right;
  int bottom;
};

bool contains(Rect rect, int x, int y) { return x >= rect.left && y >= rect.top && x < rect.right && y < rect.bottom; }

bool referenceInk(const std::vector<uint8_t>& bitmap, int source, bool twoBit, Plane plane) {
  if (!twoBit) return (bitmap[source / 8] >> (7 - source % 8)) & 1;
  // Keep the oracle's original inverted color convention independent of the
  // rasterizer's level masks and packed lookup table.
  const int value = 3 - ((bitmap[source / 4] >> ((3 - source % 4) * 2)) & 3);
  if (plane == Plane::BW) return value < 3;
  if (plane == Plane::GrayLSB) return value == 1;
  return value == 1 || value == 2;
}

void paintReference(const std::vector<uint8_t>& bitmap, int orientation, const glyphBitmap::Frame& logical, bool twoBit,
                    Plane plane, bool state, int width, int height, int originY, int rows, Rect clip, int guard,
                    std::vector<uint8_t>& expected) {
  const bool black = state && (!twoBit || plane == Plane::BW);
  const Rect band{0, originY, PANEL_WIDTH, originY + rows};
  for (int gy = 0; gy < height; ++gy) {
    for (int gx = 0; gx < width; ++gx) {
      const int lx = logical.x + gx * logical.dxX + gy * logical.dyX;
      const int ly = logical.y + gx * logical.dxY + gy * logical.dyY;
      if (!contains(clip, lx, ly)) continue;
      const auto [px, py] = physical(orientation, lx, ly);
      if (!contains(band, px, py)) continue;
      if (!referenceInk(bitmap, gy * width + gx, twoBit, plane)) continue;
      auto& byte = expected[guard + (py - originY) * STRIDE + px / 8];
      if (black)
        byte &= static_cast<uint8_t>(~(0x80 >> (px % 8)));
      else
        byte |= 0x80 >> (px % 8);
    }
  }
}

// Use array allocations with no spare vector capacity or guard bytes when a
// case requests exact bounds. Sanitizers can then catch source overreads and
// even zero-mask destination accesses immediately outside the real buffers.
void drawExact(const std::vector<uint8_t>& bitmap, int width, int height, bool twoBit, Plane plane, bool state,
               glyphBitmap::Target target, glyphBitmap::Clip clip) {
  const size_t bytes = static_cast<size_t>(target.rows) * target.stride;
  const auto source = std::make_unique<uint8_t[]>(bitmap.size());
  const auto destination = std::make_unique<uint8_t[]>(bytes);
  std::copy(bitmap.begin(), bitmap.end(), source.get());
  std::copy_n(target.buffer, bytes, destination.get());
  uint8_t* original = target.buffer;
  target.buffer = destination.get();
  glyphBitmap::draw(source.get(), width, height, twoBit, plane, state, target, clip);
  std::copy_n(destination.get(), bytes, original);
}

void compareFrame(int orientation, const glyphBitmap::Frame& logical, bool twoBit, Plane plane, bool state, int width,
                  int height, int originY, int rows, Rect clip, const std::vector<uint8_t>& bitmap, int guard = 16) {
  SCOPED_TRACE(::testing::Message() << orientation << ',' << twoBit << ',' << static_cast<int>(plane) << ',' << state
                                    << " size=" << width << 'x' << height << " at=" << logical.x << ',' << logical.y
                                    << " axes=" << logical.dxX << ',' << logical.dxY << ',' << logical.dyX << ','
                                    << logical.dyY << " band=" << originY << ',' << rows << " clip=" << clip.left << ','
                                    << clip.top << ',' << clip.right << ',' << clip.bottom << " guard=" << guard);
  // Nonuniform background and guards verify transparency and all off-band bytes.
  std::vector<uint8_t> expected(rows * STRIDE + 2 * guard);
  for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i * 53 + 0xa5);
  auto actual = expected;
  paintReference(bitmap, orientation, logical, twoBit, plane, state, width, height, originY, rows, clip, guard,
                 expected);

  glyphBitmap::Clip local{0, 0, width, height};
  glyphBitmap::clipToRect(logical, clip.left, clip.top, clip.right, clip.bottom, local);

  const auto [px, py] = physical(orientation, logical.x, logical.y);
  const auto [xx, xy] = physical(orientation, logical.x + logical.dxX, logical.y + logical.dxY);
  const auto [yx, yy] = physical(orientation, logical.x + logical.dyX, logical.y + logical.dyY);
  const glyphBitmap::Target target{
      actual.data() + guard, PANEL_WIDTH, STRIDE, originY, rows, {px, py, xx - px, xy - py, yx - px, yy - py}};
  if (guard == 0)
    drawExact(bitmap, width, height, twoBit, plane, state, target, local);
  else
    glyphBitmap::draw(bitmap.data(), width, height, twoBit, plane, state, target, local);
  EXPECT_EQ(expected, actual);
}

void compareBitmap(int orientation, int rotation, bool twoBit, Plane plane, bool state, int width, int height, int x,
                   int y, int originY, int rows, Rect clip, const std::vector<uint8_t>& bitmap, int guard = 16) {
  const auto [dxX, dxY, dyX, dyY] = AXES[rotation];
  compareFrame(orientation, {x, y, dxX, dxY, dyX, dyY}, twoBit, plane, state, width, height, originY, rows, clip,
               bitmap, guard);
}

std::vector<uint8_t> bitmapFor(int width, int height, bool twoBit, uint8_t seed) {
  std::vector<uint8_t> bitmap((width * height * (twoBit ? 2 : 1) + 7) / 8);
  for (size_t i = 0; i < bitmap.size(); ++i) bitmap[i] = static_cast<uint8_t>(i * 73 + seed);
  return bitmap;
}

void compare(int orientation, int rotation, bool twoBit, Plane plane, bool state, int width, int height, int x, int y,
             int originY, int rows, Rect clip) {
  compareBitmap(orientation, rotation, twoBit, plane, state, width, height, x, y, originY, rows, clip,
                bitmapFor(width, height, twoBit, 0x1b));
}

// The two landscape orientations give physical glyph-x steps +1 and -1.
// localClip is deliberately applied in logical space before the tested draw.
void compareHorizontal(int direction, bool twoBit, Plane plane, bool state, int width, int height, int physicalX,
                       int physicalY, int originY, int rows, Rect localClip, const std::vector<uint8_t>& bitmap,
                       int guard = 16) {
  const int orientation = direction > 0 ? 3 : 1;
  const int x = direction > 0 ? physicalX : PANEL_WIDTH - 1 - physicalX;
  const int y = direction > 0 ? physicalY : PANEL_HEIGHT - 1 - physicalY;
  compareBitmap(orientation, 0, twoBit, plane, state, width, height, x, y, originY, rows,
                {x + localClip.left, y + localClip.top, x + localClip.right, y + localClip.bottom}, bitmap, guard);
}

// Direct physical frames cover each vertical-axis sign independently, including
// mirrored frames beyond the renderer's rotations. The oracle still visits
// every source pixel and applies a physical rectangle, never a packed mask.
void compareVertical(int columnDirection, int rowDirection, bool twoBit, Plane plane, bool state, int width, int height,
                     int physicalX, int physicalY, int originY, int rows, Rect localClip,
                     const std::vector<uint8_t>& bitmap, int guard = 16) {
  const int x1 = physicalX + rowDirection * localClip.top;
  const int x2 = physicalX + rowDirection * (localClip.bottom - 1);
  const int y1 = physicalY + columnDirection * localClip.left;
  const int y2 = physicalY + columnDirection * (localClip.right - 1);
  compareFrame(3, {physicalX, physicalY, 0, columnDirection, rowDirection, 0}, twoBit, plane, state, width, height,
               originY, rows, {std::min(x1, x2), std::min(y1, y2), std::max(x1, x2) + 1, std::max(y1, y2) + 1}, bitmap,
               guard);
}

std::vector<uint8_t> packedRun(int packed, int width) {
  // Every packed byte occurs in both halves of an eight-pixel run; complement
  // and nibble-swap patterns also make joins and four-pixel leftovers distinct.
  const std::array<uint8_t, 3> bytes = {{
      static_cast<uint8_t>(packed),
      static_cast<uint8_t>(packed ^ 0xff),
      static_cast<uint8_t>((packed << 4) | (packed >> 4)),
  }};
  std::vector<uint8_t> bitmap(width / 4);
  for (size_t i = 0; i < bitmap.size(); ++i) bitmap[i] = bytes[i];
  return bitmap;
}

void compareHorizontalClipsAndBands(int direction, bool twoBit, Plane plane, bool state, int width, int height,
                                    int physicalX, int physicalY, const std::vector<uint8_t>& bitmap) {
  for (const auto [origin, rows] : {std::pair{0, PANEL_HEIGHT}, std::pair{12, 1}, std::pair{13, 2}, std::pair{16, 1}}) {
    for (Rect clip : {Rect{0, 0, width, height}, Rect{1, 0, width - 1, height}, Rect{3, 1, width - 2, height - 1}}) {
      compareHorizontal(direction, twoBit, plane, state, width, height, physicalX, physicalY, origin, rows, clip,
                        bitmap);
    }
  }
}

void compareVerticalTileExtents(int columnDirection, int rowDirection, bool twoBit, Plane plane, bool state,
                                int alignment) {
  for (int keptRows : {8, 9, 16}) {
    for (int keptColumns : {1, 2, 3, 4, 5, 6, 7}) {
      for (int sourceLeft = 0; sourceLeft < 4; ++sourceLeft) {
        const int sourceTop = sourceLeft & 1;
        const int width = sourceLeft + keptColumns + 1;
        const int height = sourceTop + keptRows + 1;
        const int physicalX = (rowDirection > 0 ? 8 + alignment : 24 + alignment) - rowDirection * sourceTop;
        const int physicalY = (columnDirection > 0 ? 8 : 24) - columnDirection * sourceLeft;
        compareVertical(columnDirection, rowDirection, twoBit, plane, state, width, height, physicalX, physicalY, 0,
                        PANEL_HEIGHT, {sourceLeft, sourceTop, sourceLeft + keptColumns, sourceTop + keptRows},
                        bitmapFor(width, height, twoBit, static_cast<uint8_t>(width * 17 + keptRows)));
      }
    }
  }
}

void compareVerticalClipsAndBands(int columnDirection, int rowDirection, bool twoBit, Plane plane, bool state,
                                  int alignment) {
  const int physicalX = rowDirection > 0 ? -3 + alignment : PANEL_WIDTH + 2 - alignment;
  const int physicalY = columnDirection > 0 ? 6 : 15;
  const auto bitmap = bitmapFor(13, 17, twoBit, 0x1b);
  for (const auto [origin, rows] : {std::pair{0, PANEL_HEIGHT}, std::pair{0, 1}, std::pair{7, 1}, std::pair{7, 2},
                                    std::pair{7, 3}, std::pair{7, 4}, std::pair{9, 7}, std::pair{28, 4}}) {
    for (Rect clip : {Rect{0, 0, 13, 17}, Rect{1, 1, 12, 16}, Rect{3, 2, 11, 16}}) {
      compareVertical(columnDirection, rowDirection, twoBit, plane, state, 13, 17, physicalX, physicalY, origin, rows,
                      clip, bitmap);
    }
  }
}

void compareVerticalExactEdges(int columnDirection, int rowDirection, bool twoBit, Plane plane, bool state,
                               int keptColumns) {
  // The intervening lengths put an eight-row tile at every framebuffer bit
  // offset against one of the two exact buffer edges, with all scalar tails.
  for (int keptRows : {8, 9, 10, 11, 12, 13, 14, 15, 16}) {
    for (int sourceLeft = 0; sourceLeft < 4; ++sourceLeft) {
      for (bool atEnd : {false, true}) {
        const int width = sourceLeft + keptColumns;
        const int left = atEnd ? PANEL_WIDTH - keptRows : 0;
        const int origin = atEnd ? PANEL_HEIGHT - keptColumns : 0;
        const int physicalX = left + (rowDirection < 0 ? keptRows - 1 : 0);
        const int physicalY = origin + (columnDirection < 0 ? keptColumns - 1 : 0) - columnDirection * sourceLeft;
        for (uint8_t seed : {0x00, 0x1b, 0x55, 0xaa, 0xe4, 0xff}) {
          SCOPED_TRACE(::testing::Message() << "seed=" << static_cast<unsigned>(seed));
          compareVertical(columnDirection, rowDirection, twoBit, plane, state, width, keptRows, physicalX, physicalY,
                          origin, keptColumns, {sourceLeft, 0, width, keptRows},
                          bitmapFor(width, keptRows, twoBit, seed), 0);
        }
      }
    }
  }
}

void compareOneBitHorizontalExactWindows(int direction, Plane plane, bool state, int sourceLeft, int keptColumns) {
  const int width = sourceLeft + keptColumns;
  for (int height = 1; height <= 7; ++height) {
    const auto bitmap = bitmapFor(width, height, false, static_cast<uint8_t>(width * 17 + height));
    for (bool atEnd : {false, true}) {
      const int left = atEnd ? PANEL_WIDTH - keptColumns : 0;
      const int origin = atEnd ? PANEL_HEIGHT - height : 0;
      const int physicalX = left + (direction < 0 ? keptColumns - 1 : 0) - direction * sourceLeft;
      const int physicalY = origin + (direction < 0 ? height - 1 : 0);
      compareHorizontal(direction, false, plane, state, width, height, physicalX, physicalY, origin, height,
                        {sourceLeft, 0, width, height}, bitmap, 0);
    }
  }
}

void compareOneBitVerticalExactWindows(int columnDirection, int rowDirection, Plane plane, bool state, int sourceLeft,
                                       int keptColumns) {
  const int width = sourceLeft + keptColumns;
  for (int height = 1; height <= 7; ++height) {
    const auto bitmap = bitmapFor(width, height, false, static_cast<uint8_t>(width * 17 + height));
    for (bool atEnd : {false, true}) {
      const int left = atEnd ? PANEL_WIDTH - height : 0;
      const int origin = atEnd ? PANEL_HEIGHT - keptColumns : 0;
      const int physicalX = left + (rowDirection < 0 ? height - 1 : 0);
      const int physicalY = origin + (columnDirection < 0 ? keptColumns - 1 : 0) - columnDirection * sourceLeft;
      compareVertical(columnDirection, rowDirection, false, plane, state, width, height, physicalX, physicalY, origin,
                      keptColumns, {sourceLeft, 0, width, height}, bitmap, 0);
    }
  }
}

}  // namespace

TEST(GlyphBitmap, MatchesPerPixelReferenceAcrossOrientationsRotationsPlanesAndClipping) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      for (bool twoBit : {false, true}) {
        for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
          for (bool state : {false, true}) {
            for (int width : {1, 3, 7, 16, 31}) {
              for (const auto [x, y] : {std::pair{-5, -3}, std::pair{0, 0}, std::pair{9, 11}, std::pair{29, 31}}) {
                for (const auto [origin, rows] :
                     {std::pair{0, 32}, std::pair{0, 7}, std::pair{7, 11}, std::pair{28, 4}}) {
                  compare(orientation, rotation, twoBit, plane, state, width, 13, x, y, origin, rows,
                          {-20, -20, 60, 60});
                  compare(orientation, rotation, twoBit, plane, state, width, 13, x, y, origin, rows, {2, 3, 18, 20});
                }
              }
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, EmptyAndFullyClippedGlyphsDoNotWrite) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      compare(orientation, rotation, true, Plane::BW, true, 0, 0, 0, 0, 0, 32, {0, 0, 40, 32});
      compare(orientation, rotation, true, Plane::BW, true, 11, 9, 100, 100, 0, 32, {0, 0, 40, 32});
      compare(orientation, rotation, true, Plane::BW, true, 11, 9, 0, 0, 0, 32, {12, 10, 15, 20});
    }
  }
}

TEST(GlyphBitmap, EveryPackedByteMatchesReferenceForHorizontalRunsPlanesStatesAndBitOffsets) {
  for (int packed = 0; packed < 256; ++packed) {
    SCOPED_TRACE(::testing::Message() << "packed=" << packed);
    for (int direction : {-1, 1}) {
      for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
        for (bool state : {false, true}) {
          for (int alignment = 0; alignment < 8; ++alignment) {
            for (int width : {4, 8, 12}) {
              const int physicalX = direction > 0 ? 8 + alignment : 24 + alignment;
              compareHorizontal(direction, true, plane, state, width, 1, physicalX, 12, 12, 1, {0, 0, width, 1},
                                packedRun(packed, width));
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, EveryPackedByteMatchesReferenceForVerticalEightRowTilesPlanesStatesAndBitOffsets) {
  for (int packed = 0; packed < 256; ++packed) {
    SCOPED_TRACE(::testing::Message() << "packed=" << packed);
    for (int columnDirection : {-1, 1}) {
      for (int rowDirection : {-1, 1}) {
        for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
          for (bool state : {false, true}) {
            for (int alignment = 0; alignment < 8; ++alignment) {
              const int physicalX = rowDirection > 0 ? 8 + alignment : 24 + alignment;
              const int physicalY = columnDirection > 0 ? 12 : 20;
              compareVertical(columnDirection, rowDirection, true, plane, state, 4, 8, physicalX, physicalY, 0,
                              PANEL_HEIGHT, {0, 0, 4, 8}, bitmapFor(4, 8, true, static_cast<uint8_t>(packed)));
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, OddWidthRowsClippedPrefixesAndTailsMatchReferenceAcrossHorizontalStripBands) {
  for (bool twoBit : {false, true}) {
    for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
      for (bool state : {false, true}) {
        for (int direction : {-1, 1}) {
          for (int alignment = 0; alignment < 8; ++alignment) {
            for (int width : {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 31}) {
              for (uint8_t seed : {0x00, 0x1b, 0x55, 0xaa, 0xe4, 0xff}) {
                SCOPED_TRACE(::testing::Message() << "seed=" << static_cast<unsigned>(seed));
                const int physicalX = direction > 0 ? 8 + alignment : 24 + alignment;
                const int physicalY = direction > 0 ? 12 : 16;
                compareHorizontalClipsAndBands(direction, twoBit, plane, state, width, 5, physicalX, physicalY,
                                               bitmapFor(width, 5, twoBit, seed));
              }
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, HorizontalRunsAtExactBufferEndMatchReferenceWithoutPadding) {
  // No trailing guard allocation here: sanitizer runs must also catch a
  // zero-mask read/modify/write beyond the last buffered byte.
  for (bool twoBit : {false, true}) {
    for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
      for (bool state : {false, true}) {
        for (int direction : {-1, 1}) {
          for (int width : {1, 4, 7, 8, 9, 11, 12, 13}) {
            for (uint8_t seed : {0x00, 0x1b, 0x55, 0xaa, 0xe4, 0xff}) {
              const int physicalX = direction > 0 ? PANEL_WIDTH - width : PANEL_WIDTH - 1;
              const int physicalY = direction > 0 ? PANEL_HEIGHT - 3 : PANEL_HEIGHT - 1;
              compareHorizontal(direction, twoBit, plane, state, width, 3, physicalX, physicalY, PANEL_HEIGHT - 3, 3,
                                {0, 0, width, 3}, bitmapFor(width, 3, twoBit, seed), 0);
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, VerticalTilesAndRowTailsMatchReferenceForEdgeGroupsOddWidthsAndClippedSourceStarts) {
  for (bool twoBit : {false, true}) {
    for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
      for (bool state : {false, true}) {
        for (int columnDirection : {-1, 1}) {
          for (int rowDirection : {-1, 1}) {
            for (int alignment = 0; alignment < 8; ++alignment) {
              compareVerticalTileExtents(columnDirection, rowDirection, twoBit, plane, state, alignment);
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, VerticalTilesMatchReferenceAcrossPanelClipsAndNarrowStripBands) {
  for (bool twoBit : {false, true}) {
    for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
      for (bool state : {false, true}) {
        for (int columnDirection : {-1, 1}) {
          for (int rowDirection : {-1, 1}) {
            for (int alignment = 0; alignment < 8; ++alignment) {
              compareVerticalClipsAndBands(columnDirection, rowDirection, twoBit, plane, state, alignment);
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, VerticalTilesMatchReferenceAtExactSourceAndDestinationBufferEndsWithoutPadding) {
  for (bool twoBit : {false, true}) {
    for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
      for (bool state : {false, true}) {
        for (int columnDirection : {-1, 1}) {
          for (int rowDirection : {-1, 1}) {
            for (int keptColumns : {1, 2, 3, 4, 5, 6, 7}) {
              compareVerticalExactEdges(columnDirection, rowDirection, twoBit, plane, state, keptColumns);
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, EightNineAndSixteenRowTilesMatchReferenceForPortraitAndRotatedLabels) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
        for (bool state : {false, true}) {
          for (int height : {8, 9, 16}) {
            for (int width : {1, 3, 5, 7}) {
              compare(orientation, rotation, true, plane, state, width, height, 12, 12, 0, PANEL_HEIGHT,
                      {-20, -20, 60, 60});
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, EveryOneBitByteMatchesHorizontalReferenceForShortChunksStatesPlanesAndDestinationOffsets) {
  for (int packed = 0; packed < 256; ++packed) {
    SCOPED_TRACE(::testing::Message() << "packed=" << packed);
    const std::vector<uint8_t> bitmap{static_cast<uint8_t>(packed)};
    for (int direction : {-1, 1}) {
      for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
        for (bool state : {false, true}) {
          for (int alignment = 0; alignment < 8; ++alignment) {
            for (int keptColumns = 1; keptColumns <= 8; ++keptColumns) {
              const int physicalX = direction > 0 ? 8 + alignment : 24 + alignment;
              compareHorizontal(direction, false, plane, state, 8, 1, physicalX, 12, 12, 1, {0, 0, keptColumns, 1},
                                bitmap, 0);
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, EveryOneBitByteMatchesVerticalReferenceForShortChunksStatesPlanesAndDestinationOffsets) {
  for (int packed = 0; packed < 256; ++packed) {
    SCOPED_TRACE(::testing::Message() << "packed=" << packed);
    const std::vector<uint8_t> bitmap{static_cast<uint8_t>(packed)};
    for (int columnDirection : {-1, 1}) {
      for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
        for (bool state : {false, true}) {
          for (int alignment = 0; alignment < 8; ++alignment) {
            for (int keptColumns = 1; keptColumns <= 8; ++keptColumns) {
              const int physicalY = 7 + (columnDirection < 0 ? keptColumns - 1 : 0);
              compareVertical(columnDirection, 1, false, plane, state, 8, 1, alignment, physicalY, 7, keptColumns,
                              {0, 0, keptColumns, 1}, bitmap, 0);
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, OneBitShortHorizontalGlyphsMatchReferenceForEverySourcePrefixAndExactBufferEdges) {
  for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
    for (bool state : {false, true}) {
      for (int direction : {-1, 1}) {
        for (int sourceLeft = 0; sourceLeft < 8; ++sourceLeft) {
          for (int keptColumns = 1; keptColumns <= 17; ++keptColumns) {
            compareOneBitHorizontalExactWindows(direction, plane, state, sourceLeft, keptColumns);
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, OneBitShortVerticalGlyphsMatchReferenceForEverySourcePrefixSignedAxesAndExactBufferEdges) {
  for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
    for (bool state : {false, true}) {
      for (int columnDirection : {-1, 1}) {
        for (int rowDirection : {-1, 1}) {
          for (int sourceLeft = 0; sourceLeft < 8; ++sourceLeft) {
            for (int keptColumns = 1; keptColumns <= 17; ++keptColumns) {
              compareOneBitVerticalExactWindows(columnDirection, rowDirection, plane, state, sourceLeft, keptColumns);
            }
          }
        }
      }
    }
  }
}
