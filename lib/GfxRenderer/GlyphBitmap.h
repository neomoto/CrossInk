#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// Glyph rasterizer that resolves orientation, clipping and framebuffer
// addressing once per glyph instead of once per pixel. It has no dependency
// on GfxRenderer so it can be unit-tested on the host.
namespace glyphBitmap {

// Framebuffer plane a glyph is painted into.
enum class Plane : uint8_t { BW, GrayLSB, GrayMSB };

// Each entry holds the four painted pixels in forward order in its high
// nibble and reverse order in its low nibble. All three planes use 768 bytes
// of read-only data, generated at compile time.
inline constexpr std::array<uint8_t, 3> INK_LEVELS = {{0x0e, 0x04, 0x06}};

constexpr std::array<std::array<uint8_t, 256>, 3> makePackedInkMasks() {
  std::array<std::array<uint8_t, 256>, 3> masks{};
  for (unsigned plane = 0; plane < masks.size(); ++plane) {
    for (unsigned packed = 0; packed < 256; ++packed) {
      for (unsigned pixel = 0; pixel < 4; ++pixel) {
        const unsigned ink = (packed >> (6 - pixel * 2)) & 3;
        if ((INK_LEVELS[plane] & (1u << ink)) != 0) {
          masks[plane][packed] |= static_cast<uint8_t>((0x80u >> pixel) | (1u << pixel));
        }
      }
    }
  }
  return masks;
}

inline constexpr auto PACKED_INK_MASKS = makePackedInkMasks();

// Position of glyph pixel (0, 0) plus the step for one move along the glyph's
// x and y axes, all in one coordinate space. The axes must describe an
// orthogonal unit rotation: each is +/-1 on exactly one component.
struct Frame {
  int x;
  int y;
  int dxX;
  int dxY;
  int dyX;
  int dyY;
};

// Where a glyph lands in physical framebuffer space.
struct Target {
  uint8_t* buffer;  // First byte of row originY (framebuffer or strip scratch)
  int width;        // Physical panel width in pixels
  int stride;       // Bytes per physical row
  int originY;      // First physical row held by buffer
  int rows;         // Number of physical rows held by buffer
  Frame frame;      // Physical placement of the glyph
};

// Half-open rectangle in glyph-local pixel coordinates.
struct Clip {
  int left;
  int top;
  int right;
  int bottom;
};

// Narrow [start, end) on one glyph axis so that base + i * step stays within
// [lower, upper). step is +1 or -1 because the transform is orthogonal.
inline void clipAxis(int base, int step, int lower, int upper, int& start, int& end) {
  if (step > 0) {
    start = std::max(start, lower - base);
    end = std::min(end, upper - base);
  } else {
    start = std::max(start, base - upper + 1);
    end = std::min(end, base - lower + 1);
  }
}

// Narrow a glyph-local clip so every kept pixel maps inside the half-open
// rectangle [left, right) x [top, bottom) of the space frame is expressed in.
// Which glyph axis runs along that space's x depends on the rotation:
// dxX != 0 means glyph x does. Runs twice per glyph, so it stays inline
// under -Os.
__attribute__((always_inline)) inline void clipToRect(const Frame& frame, int left, int top, int right, int bottom,
                                                      Clip& clip) {
  if (frame.dxX != 0) {
    clipAxis(frame.x, frame.dxX, left, right, clip.left, clip.right);
    clipAxis(frame.y, frame.dyY, top, bottom, clip.top, clip.bottom);
  } else {
    clipAxis(frame.x, frame.dyX, left, right, clip.top, clip.bottom);
    clipAxis(frame.y, frame.dxY, top, bottom, clip.left, clip.right);
  }
}

// Keep pixel writes inline when decoding a group of four pixels.
__attribute__((always_inline)) inline void paint(uint8_t* buffer, int destination, uint8_t ink, uint8_t levels,
                                                 bool clearBits) {
  if ((levels & (1u << ink)) == 0) return;
  const uint8_t mask = 0x80u >> (destination & 7);
  if (clearBits)
    buffer[destination >> 3] &= static_cast<uint8_t>(~mask);
  else
    buffer[destination >> 3] |= mask;
}

__attribute__((always_inline)) inline void paintMaskedByte(uint8_t* buffer, int index, uint8_t mask, bool clearBits) {
  // In particular, do not touch a second byte for an aligned run ending at
  // the end of the framebuffer or strip.
  if (mask == 0) return;
  if (clearBits)
    buffer[index] &= static_cast<uint8_t>(~mask);
  else
    buffer[index] |= mask;
}

// mask is MSB-first starting at the leftmost physical pixel of a horizontal
// run. Short runs occupy only its high bits. Clipping has already guaranteed
// that every nonzero bit belongs to the buffered row.
__attribute__((always_inline)) inline void paintHorizontalMask(uint8_t* buffer, int destination, uint8_t mask,
                                                               bool clearBits) {
  const uint16_t positioned = static_cast<uint16_t>(mask) << (8 - (destination & 7));
  paintMaskedByte(buffer, destination >> 3, static_cast<uint8_t>(positioned >> 8), clearBits);
  paintMaskedByte(buffer, (destination >> 3) + 1, static_cast<uint8_t>(positioned), clearBits);
}

inline uint8_t reverseOneBitByte(uint8_t bits) {
  bits = static_cast<uint8_t>(((bits & 0x55u) << 1) | ((bits >> 1) & 0x55u));
  bits = static_cast<uint8_t>(((bits & 0x33u) << 2) | ((bits >> 2) & 0x33u));
  return static_cast<uint8_t>((bits << 4) | (bits >> 4));
}

// Glyph x runs along physical y: every kept source bit has the same target
// bit mask, and successive pixels are exactly one signed row stride apart.
// writeMask is already complemented when clearing. Skip transparent pixels
// without decoding an ink level or recomputing framebuffer bit addresses.
template <bool ClearBits>
inline void paintOneBitVerticalChunk(uint8_t* buffer, int index, int stepBytes, uint8_t writeMask, uint8_t bits) {
  while (bits != 0) {
    if ((bits & 0x80u) != 0) {
      if constexpr (ClearBits)
        buffer[index] &= writeMask;
      else
        buffer[index] |= writeMask;
    }
    bits = static_cast<uint8_t>(bits << 1);
    index += stepBytes;
  }
}

// HorizontalStep is +/-1 for a physical horizontal row, or 0 for a vertical
// row. Load each source-byte chunk once, including clipped prefixes and
// bit-contiguous odd-width rows; never read a byte beyond the kept chunk.
template <int HorizontalStep, bool ClearBits>
inline void drawOneBitRow(const uint8_t* bitmap, int source, int rowBit, int remaining, int stepBytes,
                          uint8_t* buffer) {
  const uint8_t* sourceByte = bitmap + (source >> 3);
  int offset = source & 7;
  int destination = rowBit;
  const uint8_t pixelMask = static_cast<uint8_t>(0x80u >> (rowBit & 7));
  const uint8_t writeMask = ClearBits ? static_cast<uint8_t>(~pixelMask) : pixelMask;
  if constexpr (HorizontalStep == 0) destination >>= 3;
  while (remaining > 0) {
    const int columns = std::min(remaining, 8 - offset);
    uint8_t bits = static_cast<uint8_t>(*sourceByte++ << offset) & static_cast<uint8_t>(0xffu << (8 - columns));
    if constexpr (HorizontalStep == 0) {
      paintOneBitVerticalChunk<ClearBits>(buffer, destination, stepBytes, writeMask, bits);
      destination += columns * stepBytes;
    } else {
      if constexpr (HorizontalStep < 0) {
        bits = static_cast<uint8_t>(reverseOneBitByte(bits) << (8 - columns));
        paintHorizontalMask(buffer, destination - columns + 1, bits, ClearBits);
      } else {
        paintHorizontalMask(buffer, destination, bits, ClearBits);
      }
      destination += columns * HorizontalStep;
    }
    remaining -= columns;
    offset = 0;
  }
}

template <int HorizontalStep>
inline void drawOneBitRows(const uint8_t* bitmap, int width, const Clip& clip, int rowBit, int stepY, int stepBytes,
                           bool clearBits, uint8_t* buffer) {
  for (int y = clip.top; y < clip.bottom; ++y, rowBit += stepY) {
    if (clearBits)
      drawOneBitRow<HorizontalStep, true>(bitmap, y * width + clip.left, rowBit, clip.right - clip.left, stepBytes,
                                          buffer);
    else
      drawOneBitRow<HorizontalStep, false>(bitmap, y * width + clip.left, rowBit, clip.right - clip.left, stepBytes,
                                           buffer);
  }
}

__attribute__((always_inline)) inline void drawScalarPixels(const uint8_t* bitmap, int source, int destination,
                                                            int remaining, int stepX, uint8_t levels, bool clearBits,
                                                            uint8_t* buffer) {
  while (remaining--) {
    const uint8_t ink = (bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3;
    paint(buffer, destination, ink, levels, clearBits);
    ++source;
    destination += stepX;
  }
}

__attribute__((always_inline)) inline void drawScalarRow(const uint8_t* bitmap, int source, int destination,
                                                         int remaining, int stepX, uint8_t levels, bool clearBits,
                                                         uint8_t* buffer) {
  // Rows are bit-contiguous, not byte-padded. Align after any clipped prefix.
  while (remaining && (source & 3)) {
    paint(buffer, destination, (bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3, levels, clearBits);
    ++source;
    destination += stepX;
    --remaining;
  }
  while (remaining >= 4) {
    const uint8_t packed = bitmap[source >> 2];
    paint(buffer, destination, packed >> 6, levels, clearBits);
    paint(buffer, destination + stepX, (packed >> 4) & 3, levels, clearBits);
    paint(buffer, destination + 2 * stepX, (packed >> 2) & 3, levels, clearBits);
    paint(buffer, destination + 3 * stepX, packed & 3, levels, clearBits);
    source += 4;
    destination += 4 * stepX;
    remaining -= 4;
  }
  drawScalarPixels(bitmap, source, destination, remaining, stepX, levels, clearBits, buffer);
}

template <int Step>
__attribute__((always_inline)) inline void drawHorizontalRow(const uint8_t* bitmap, const uint8_t* masks, int source,
                                                             int destination, int remaining, uint8_t levels,
                                                             bool clearBits, uint8_t* buffer) {
  const int prefix = std::min(remaining, (4 - (source & 3)) & 3);
  drawScalarPixels(bitmap, source, destination, prefix, Step, levels, clearBits, buffer);
  source += prefix;
  destination += prefix * Step;
  remaining -= prefix;

  while (remaining >= 8) {
    const uint8_t first = masks[bitmap[source >> 2]];
    const uint8_t second = masks[bitmap[(source >> 2) + 1]];
    uint8_t mask;
    if constexpr (Step > 0)
      mask = (first & 0xf0) | (second >> 4);
    else
      mask = static_cast<uint8_t>((second << 4) | (first & 0x0f));
    paintHorizontalMask(buffer, destination + (Step > 0 ? 0 : -7), mask, clearBits);
    source += 8;
    destination += 8 * Step;
    remaining -= 8;
  }
  if (remaining >= 4) {
    uint8_t mask = masks[bitmap[source >> 2]];
    if constexpr (Step < 0) mask = static_cast<uint8_t>(mask << 4);
    paintHorizontalMask(buffer, destination + (Step > 0 ? 0 : -3), mask & 0xf0, clearBits);
    source += 4;
    destination += 4 * Step;
    remaining -= 4;
  }
  drawScalarPixels(bitmap, source, destination, remaining, Step, levels, clearBits, buffer);
}

template <int Step>
inline void drawHorizontalRows(const uint8_t* bitmap, int width, const uint8_t* masks, const Clip& clip, int rowBit,
                               int stepY, uint8_t levels, bool clearBits, uint8_t* buffer) {
  for (int y = clip.top; y < clip.bottom; ++y, rowBit += stepY) {
    drawHorizontalRow<Step>(bitmap, masks, y * width + clip.left, rowBit, clip.right - clip.left, levels, clearBits,
                            buffer);
  }
}

// Decode only the kept columns. Odd-width, bit-contiguous rows can put this
// group across two source bytes; an edge group may finish in the first byte.
__attribute__((always_inline)) inline uint8_t readVerticalRowMask(const uint8_t* bitmap, const uint8_t* masks,
                                                                  int source, int columns) {
  const int shift = (source & 3) * 2;
  uint8_t packed = static_cast<uint8_t>(bitmap[source >> 2] << shift);
  if ((source & 3) + columns > 4) {
    packed |= bitmap[(source >> 2) + 1] >> (8 - shift);
  }
  return masks[packed] & static_cast<uint8_t>(0xffu << (8 - columns));
}

// Four MSB-first row nibbles become four MSB-first column nibbles. Swap the
// two row/column address-bit pairs without a table or temporary pixel array.
__attribute__((always_inline)) inline uint16_t transposeFourByFour(uint16_t matrix) {
  uint16_t swap = (matrix ^ (matrix >> 3)) & 0x0a0a;
  matrix ^= swap ^ (swap << 3);
  swap = (matrix ^ (matrix >> 6)) & 0x00cc;
  return matrix ^ swap ^ (swap << 6);
}

__attribute__((always_inline)) inline uint16_t gatherVerticalColumns(const uint8_t* bitmap, const uint8_t* masks,
                                                                     int source, int sourceStep, int columns) {
  uint16_t matrix = 0;
  for (int row = 0; row < 4; ++row, source += sourceStep) {
    matrix = static_cast<uint16_t>((matrix << 4) | (readVerticalRowMask(bitmap, masks, source, columns) >> 4));
  }
  return transposeFourByFour(matrix);
}

__attribute__((always_inline)) inline void drawVerticalTile(const uint8_t* bitmap, const uint8_t* masks, int source,
                                                            int sourceStep, int destination, int stepX, int columns,
                                                            bool clearBits, uint8_t* buffer) {
  uint16_t first = gatherVerticalColumns(bitmap, masks, source, sourceStep, columns);
  uint16_t second = gatherVerticalColumns(bitmap, masks, source + 4 * sourceStep, sourceStep, columns);
  for (int column = 0; column < columns; ++column, destination += stepX) {
    const uint8_t mask = static_cast<uint8_t>(((first >> 8) & 0xf0) | (second >> 12));
    paintHorizontalMask(buffer, destination, mask, clearBits);
    first = static_cast<uint16_t>(first << 4);
    second = static_cast<uint16_t>(second << 4);
  }
}

inline void drawVerticalRows(const uint8_t* bitmap, int width, const uint8_t* masks, const Clip& clip, int rowBit,
                             int stepX, int stepY, uint8_t levels, bool clearBits, uint8_t* buffer) {
  // Gather eight glyph rows in physical-left-to-right order, including when
  // glyph y runs backwards. Only two scalar 4x4 matrices are needed per tile.
  const bool reverse = stepY < 0;
  const int sourceStep = reverse ? -width : width;
  const int sourceOffset = reverse ? 7 * width : 0;
  const int destinationOffset = reverse ? -7 : 0;
  int y = clip.top;
  for (; y + 8 <= clip.bottom; y += 8, rowBit += 8 * stepY) {
    int source = y * width + clip.left + sourceOffset;
    int destination = rowBit + destinationOffset;
    for (int x = clip.left; x < clip.right; x += 4, source += 4, destination += 4 * stepX) {
      drawVerticalTile(bitmap, masks, source, sourceStep, destination, stepX, std::min(4, clip.right - x), clearBits,
                       buffer);
    }
  }
  for (; y < clip.bottom; ++y, rowBit += stepY) {
    drawScalarRow(bitmap, y * width + clip.left, rowBit, clip.right - clip.left, stepX, levels, clearBits, buffer);
  }
}

// Paint a packed glyph into target.
//
// bitmap: rows are contiguous, MSB first, 1 or 2 bits per pixel; widths need
//   not be byte-aligned. 2bpp values are 0=white, 1=light gray, 2=dark gray,
//   3=black. 1bpp value 1 is ink.
// plane: which 2bpp values are painted. BW paints every non-white value and
//   honours state (true clears the bit, i.e. black). The gray planes only ever
//   set bits, because there 0 means leave alone and 1 means update: MSB paints
//   both grays, LSB paints dark gray only. 1bpp glyphs paint ink with state on
//   every plane.
// Clipping happens before pixel decoding, so fully hidden glyphs cost nothing.
inline void draw(const uint8_t* bitmap, int width, int height, bool twoBit, Plane plane, bool state,
                 const Target& target, Clip clip) {
  // Bit n of levels set means source value n is painted.
  uint8_t levels = 0x02;
  bool clearBits = state;
  if (twoBit) {
    levels = INK_LEVELS[static_cast<unsigned>(plane)];
    if (plane != Plane::BW) clearBits = false;
  }

  // Intersect with the glyph bounds, then with the panel width and the
  // buffered row band.
  clip.left = std::max(clip.left, 0);
  clip.top = std::max(clip.top, 0);
  clip.right = std::min(clip.right, width);
  clip.bottom = std::min(clip.bottom, height);
  clipToRect(target.frame, 0, target.originY, target.width, target.originY + target.rows, clip);
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;

  // Walk the framebuffer as a flat bit index. One glyph column advances
  // stepX bits and one glyph row advances stepY bits; each is +/-1 for the
  // axis that maps to physical x, or +/-strideBits for physical y.
  const Frame& frame = target.frame;
  const int strideBits = target.stride * 8;
  const int stepX = frame.dxY * strideBits + frame.dxX;
  const int stepY = frame.dyY * strideBits + frame.dyX;
  int rowBit = (frame.y - target.originY) * strideBits + frame.x + clip.left * stepX + clip.top * stepY;
  if (twoBit) {
    const uint8_t* masks = PACKED_INK_MASKS[static_cast<unsigned>(plane)].data();
    if (stepX == 1) {
      drawHorizontalRows<1>(bitmap, width, masks, clip, rowBit, stepY, levels, clearBits, target.buffer);
      return;
    }
    if (stepX == -1) {
      drawHorizontalRows<-1>(bitmap, width, masks, clip, rowBit, stepY, levels, clearBits, target.buffer);
      return;
    }
    drawVerticalRows(bitmap, width, masks, clip, rowBit, stepX, stepY, levels, clearBits, target.buffer);
    return;
  }
  if (stepX == 1) {
    drawOneBitRows<1>(bitmap, width, clip, rowBit, stepY, 0, clearBits, target.buffer);
    return;
  }
  if (stepX == -1) {
    drawOneBitRows<-1>(bitmap, width, clip, rowBit, stepY, 0, clearBits, target.buffer);
    return;
  }
  drawOneBitRows<0>(bitmap, width, clip, rowBit, stepY, frame.dxY * target.stride, clearBits, target.buffer);
}

}  // namespace glyphBitmap
