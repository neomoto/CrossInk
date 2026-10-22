#pragma once

#include "GlyphBitmap.h"

// Rasterizes one normalized Bitmap::readNextRow() row without a second row
// buffer. Unlike glyph data, these pixels are 0=black through 3=white, and
// each row starts at a byte boundary.
namespace bitmapRow {

enum class Plane : uint8_t { BW, OverlayLSB, OverlayMSB, AbsoluteLSB, AbsoluteMSB };

template <Plane plane>
inline constexpr bool ABSOLUTE = plane == Plane::AbsoluteLSB || plane == Plane::AbsoluteMSB;

// Select the low bit of each 2bpp pixel, then compact the four selected bits
// into an MSB-first nibble. Absolute planes encode white bits; other planes
// encode pixels to paint (black for BW, white for grayscale overlays).
template <Plane plane>
__attribute__((always_inline)) inline uint8_t pixelMask(uint8_t packed) {
  uint8_t bits;
  if constexpr (plane == Plane::BW)
    bits = static_cast<uint8_t>(~(packed & (packed >> 1)));
  else if constexpr (plane == Plane::OverlayLSB)
    bits = packed & static_cast<uint8_t>(~(packed >> 1));
  else if constexpr (plane == Plane::OverlayMSB)
    bits = packed ^ (packed >> 1);
  else if constexpr (plane == Plane::AbsoluteMSB)
    bits = packed >> 1;
  else
    bits = packed;
  bits &= 0x55;
  bits = static_cast<uint8_t>((bits | (bits >> 1)) & 0x33);
  return static_cast<uint8_t>(((bits | (bits >> 2)) & 0x0f) << 4);
}

__attribute__((always_inline)) inline void replaceMaskedByte(uint8_t* buffer, int index, uint8_t mask, uint8_t bits) {
  if (mask == 0) return;
  buffer[index] = static_cast<uint8_t>((buffer[index] & ~mask) | (bits & mask));
}

template <Plane plane, int Step>
__attribute__((always_inline)) inline void paintHorizontal(uint8_t* buffer, int destination, uint8_t bits,
                                                           int columns) {
  if constexpr (Step < 0) {
    bits = static_cast<uint8_t>(glyphBitmap::reverseOneBitByte(bits) << (8 - columns));
    destination -= columns - 1;
  }
  if constexpr (ABSOLUTE<plane>) {
    const int shift = 8 - (destination & 7);
    const uint8_t coverage = static_cast<uint8_t>(0xffu << (8 - columns));
    const uint16_t mask = static_cast<uint16_t>(coverage) << shift;
    const uint16_t positioned = static_cast<uint16_t>(bits) << shift;
    replaceMaskedByte(buffer, destination >> 3, static_cast<uint8_t>(mask >> 8), static_cast<uint8_t>(positioned >> 8));
    replaceMaskedByte(buffer, (destination >> 3) + 1, static_cast<uint8_t>(mask), static_cast<uint8_t>(positioned));
  } else {
    bits &= static_cast<uint8_t>(0xffu << (8 - columns));
    glyphBitmap::paintHorizontalMask(buffer, destination, bits, plane == Plane::BW);
  }
}

template <Plane plane, int Step>
inline void drawHorizontal(const uint8_t* row, int source, int destination, int remaining, uint8_t* buffer) {
  const int prefix = std::min(remaining, (4 - (source & 3)) & 3);
  if (prefix != 0) {
    const uint8_t bits = static_cast<uint8_t>(pixelMask<plane>(row[source >> 2]) << (source & 3));
    paintHorizontal<plane, Step>(buffer, destination, bits, prefix);
    source += prefix;
    destination += prefix * Step;
    remaining -= prefix;
  }
  while (remaining >= 8) {
    const uint8_t bits = pixelMask<plane>(row[source >> 2]) | (pixelMask<plane>(row[(source >> 2) + 1]) >> 4);
    paintHorizontal<plane, Step>(buffer, destination, bits, 8);
    source += 8;
    destination += 8 * Step;
    remaining -= 8;
  }
  if (remaining >= 4) {
    paintHorizontal<plane, Step>(buffer, destination, pixelMask<plane>(row[source >> 2]), 4);
    source += 4;
    destination += 4 * Step;
    remaining -= 4;
  }
  if (remaining != 0) paintHorizontal<plane, Step>(buffer, destination, pixelMask<plane>(row[source >> 2]), remaining);
}

__attribute__((always_inline)) inline void replaceVerticalPixel(uint8_t* buffer, int index, uint8_t mask,
                                                                uint8_t bits) {
  buffer[index] = static_cast<uint8_t>((buffer[index] & ~mask) | ((bits & 0x80u) != 0 ? mask : 0));
}

template <Plane plane>
__attribute__((always_inline)) inline void paintVerticalEdge(uint8_t* buffer, int index, int stepBytes, uint8_t mask,
                                                             uint8_t bits, int columns) {
  if constexpr (ABSOLUTE<plane>) {
    for (int column = 0; column < columns; ++column, index += stepBytes, bits = static_cast<uint8_t>(bits << 1)) {
      replaceVerticalPixel(buffer, index, mask, bits);
    }
  } else {
    bits &= static_cast<uint8_t>(0xffu << (8 - columns));
    const uint8_t writeMask = plane == Plane::BW ? static_cast<uint8_t>(~mask) : mask;
    glyphBitmap::paintOneBitVerticalChunk<plane == Plane::BW>(buffer, index, stepBytes, writeMask, bits);
  }
}

template <Plane plane>
inline void drawVertical(const uint8_t* row, int source, int destination, int remaining, int stepBytes,
                         uint8_t* buffer) {
  const uint8_t mask = static_cast<uint8_t>(0x80u >> (destination & 7));
  int index = destination >> 3;
  const int prefix = std::min(remaining, (4 - (source & 3)) & 3);
  if (prefix != 0) {
    const uint8_t bits = static_cast<uint8_t>(pixelMask<plane>(row[source >> 2]) << (source & 3));
    paintVerticalEdge<plane>(buffer, index, stepBytes, mask, bits, prefix);
    source += prefix;
    index += prefix * stepBytes;
    remaining -= prefix;
  }
  while (remaining >= 4) {
    const uint8_t bits = pixelMask<plane>(row[source >> 2]);
    if constexpr (ABSOLUTE<plane>) {
      // Portrait cover rows scatter into one fixed bit of four physical rows.
      // No per-pixel transform, clipping, plane selection or bit addressing.
      replaceVerticalPixel(buffer, index, mask, bits);
      replaceVerticalPixel(buffer, index + stepBytes, mask, static_cast<uint8_t>(bits << 1));
      replaceVerticalPixel(buffer, index + 2 * stepBytes, mask, static_cast<uint8_t>(bits << 2));
      replaceVerticalPixel(buffer, index + 3 * stepBytes, mask, static_cast<uint8_t>(bits << 3));
    } else {
      const uint8_t writeMask = plane == Plane::BW ? static_cast<uint8_t>(~mask) : mask;
      glyphBitmap::paintOneBitVerticalChunk<plane == Plane::BW>(buffer, index, stepBytes, writeMask, bits);
    }
    source += 4;
    index += 4 * stepBytes;
    remaining -= 4;
  }
  if (remaining != 0) {
    paintVerticalEdge<plane>(buffer, index, stepBytes, mask, pixelMask<plane>(row[source >> 2]), remaining);
  }
}

template <Plane plane>
inline void drawClipped(const uint8_t* row, int source, int remaining, int destination,
                        const glyphBitmap::Target& target) {
  if (target.frame.dxX == 1)
    drawHorizontal<plane, 1>(row, source, destination, remaining, target.buffer);
  else if (target.frame.dxX == -1)
    drawHorizontal<plane, -1>(row, source, destination, remaining, target.buffer);
  else
    drawVertical<plane>(row, source, destination, remaining, target.frame.dxY * target.stride, target.buffer);
}

// target.frame places source pixel zero in physical coordinates. Only kept
// source columns are read; row tail/padding and untouched destination bits are
// never painted. The target can be the full framebuffer or a physical strip.
inline void draw(const uint8_t* row, int width, int left, int right, Plane plane, const glyphBitmap::Target& target) {
  glyphBitmap::Clip clip{std::max(left, 0), 0, std::min(right, width), 1};
  glyphBitmap::clipToRect(target.frame, 0, target.originY, target.width, target.originY + target.rows, clip);
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;
  const int strideBits = target.stride * 8;
  const int step = target.frame.dxX + target.frame.dxY * strideBits;
  const int destination = (target.frame.y - target.originY) * strideBits + target.frame.x + clip.left * step;
  const int remaining = clip.right - clip.left;
  switch (plane) {
    case Plane::BW:
      drawClipped<Plane::BW>(row, clip.left, remaining, destination, target);
      break;
    case Plane::OverlayLSB:
      drawClipped<Plane::OverlayLSB>(row, clip.left, remaining, destination, target);
      break;
    case Plane::OverlayMSB:
      drawClipped<Plane::OverlayMSB>(row, clip.left, remaining, destination, target);
      break;
    case Plane::AbsoluteLSB:
      drawClipped<Plane::AbsoluteLSB>(row, clip.left, remaining, destination, target);
      break;
    case Plane::AbsoluteMSB:
      drawClipped<Plane::AbsoluteMSB>(row, clip.left, remaining, destination, target);
      break;
  }
}

}  // namespace bitmapRow
