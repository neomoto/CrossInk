#include "EpdFontFamily.h"

#include <Utf8.h>

#include <algorithm>

namespace {

struct GlyphMetrics {
  int width;
  int height;
  uint16_t advanceX;
  int left;
  int top;
};

// The original codepoint has already missed. Keep aliases ahead of synthetic
// fallbacks, and retain the resolved alias glyph rather than looking it up again.
uint32_t normalizeMissingCodepoint(const EpdFontFamily& family, const uint32_t cp, const EpdFontFamily::Style style,
                                   const EpdGlyph*& glyph) {
  const uint32_t aliasCp = syntheticGlyph::aliasCodepoint(cp);
  if (aliasCp != cp) {
    glyph = family.findGlyphData(aliasCp, style).glyph;
    return glyph ? aliasCp : REPLACEMENT_GLYPH;
  }
  if (syntheticGlyph::isSpaceFallback(cp)) return cp;
  if (syntheticGlyph::isSolid(cp) || syntheticGlyph::isGreekFallback(cp)) return cp;
  return REPLACEMENT_GLYPH;
}

const EpdGlyph* resolveDimensionGlyph(const EpdFontFamily& family, uint32_t& cp, const EpdFontFamily::Style style,
                                      const bool isCombining) {
  const EpdGlyph* glyph = family.findGlyphData(cp, style).glyph;
  if (glyph) return glyph;
  // Missing marks use only a real replacement glyph, anchored as the original
  // mark; they must not acquire a synthetic replacement or become base glyphs.
  if (isCombining) return family.findGlyphData(REPLACEMENT_GLYPH, style).glyph;

  const uint32_t originalCp = cp;
  cp = normalizeMissingCodepoint(family, cp, style, glyph);
  if (glyph || cp == originalCp) return glyph;
  return family.findGlyphData(cp, style).glyph;
}

bool measureSyntheticBase(const EpdFontFamily& family, const uint32_t cp, const EpdFontFamily::Style style,
                          const bool measureVertical, GlyphMetrics& metrics) {
  if (syntheticGlyph::isSpaceFallback(cp)) return false;

  const EpdFontData* data = family.getData(style);
  const EpdGlyph* emGlyph = family.getGlyph('M', style);
  if (syntheticGlyph::isSolid(cp)) {
    const uint16_t advanceX = syntheticGlyph::solidAdvanceX(data, emGlyph);
    const int height = syntheticGlyph::solidHeight(data, cp);
    const int width = syntheticGlyph::solidWidth(cp, advanceX, height);
    metrics = {width, height, advanceX, syntheticGlyph::solidLeft(cp, advanceX, width),
               measureVertical ? syntheticGlyph::solidTop(data, cp, height) : 0};
    return true;
  }
  if (syntheticGlyph::isGreekFallback(cp)) {
    const uint16_t advanceX = syntheticGlyph::greekAdvanceX(data, emGlyph, cp);
    const int height = syntheticGlyph::greekHeight(data, cp);
    const int width = syntheticGlyph::greekWidth(cp, advanceX, height);
    metrics = {width, height, advanceX, syntheticGlyph::greekLeft(cp, advanceX, width),
               measureVertical ? syntheticGlyph::greekTop(data, cp, height) : 0};
    return true;
  }
  if (syntheticGlyph::isReplacementFallback(cp)) {
    const uint16_t advanceX = syntheticGlyph::replacementAdvanceX(data, emGlyph);
    const int height = syntheticGlyph::replacementHeight(data);
    const int width = syntheticGlyph::replacementWidth(advanceX, height);
    metrics = {width, height, advanceX, syntheticGlyph::replacementLeft(advanceX, width),
               measureVertical ? syntheticGlyph::replacementTop(data, height) : 0};
    return true;
  }
  return false;
}

bool measureDimensionGlyph(const EpdFontFamily& family, uint32_t& cp, const EpdFontFamily::Style style,
                           const bool isCombining, const bool measureVertical, GlyphMetrics& metrics) {
  if (const EpdGlyph* glyph = resolveDimensionGlyph(family, cp, style, isCombining)) {
    // Dynamic handlers can return ring-buffer storage. Consume only the needed
    // metrics now, before kerning (or any other callback) can reuse that storage.
    metrics = {glyph->width, measureVertical ? glyph->height : 0, glyph->advanceX, glyph->left,
               measureVertical ? glyph->top : 0};
    return true;
  }
  if (isCombining) return false;
  return measureSyntheticBase(family, cp, style, measureVertical, metrics);
}

struct TextCursor {
  int x = 0;
  int left = 0;
  int width = 0;
  int top = 0;
  int32_t advanceFP = 0;
  uint32_t previousCp = 0;

  void clearBase() {
    x += fp4::toPixel(advanceFP);
    previousCp = 0;
    advanceFP = 0;
    left = 0;
    width = 0;
    top = 0;
  }

  void advance(const EpdFontFamily& family, const uint32_t cp, const EpdFontFamily::Style style) {
    if (previousCp != 0) {
      const auto kernFP = family.getKerning(previousCp, cp, style);
      x += fp4::toPixel(advanceFP + kernFP);
    }
  }

  void setBase(const uint32_t cp, const GlyphMetrics& metrics) {
    left = metrics.left;
    width = metrics.width;
    top = metrics.top;
    advanceFP = metrics.advanceX;
    previousCp = cp;
  }
};

struct TextBounds {
  TextBounds(const bool measureVertical, const bool measureInk)
      : measureVertical(measureVertical), measureInk(measureInk) {}

  int minX = 0;
  int minY = 0;
  int maxX = 0;
  int maxY = 0;
  int inkMinY = 0;
  int inkMaxY = 0;
  bool hasInk = false;
  const bool measureVertical;
  const bool measureInk;

  void includeVertical(const int bottom, const int top) {
    minY = std::min(minY, bottom);
    maxY = std::max(maxY, top);
    if (!measureInk || top <= bottom) return;
    inkMinY = hasInk ? std::min(inkMinY, bottom) : bottom;
    inkMaxY = hasInk ? std::max(inkMaxY, top) : top;
    hasInk = true;
  }

  void includeGlyph(const GlyphMetrics& metrics, const int x, const int y) {
    minX = std::min(minX, x + metrics.left);
    maxX = std::max(maxX, x + metrics.left + metrics.width);
    if (measureVertical) includeVertical(y + metrics.top - metrics.height, y + metrics.top);
  }

  void includeMark(const uint32_t cp, const GlyphMetrics& metrics, const TextCursor& cursor) {
    const auto anchor = combiningMark::anchorFor(cp);
    const int raiseBy =
        measureVertical ? combiningMark::raiseAboveBase(anchor, metrics.top, metrics.height, cursor.top) : 0;
    const int x = combiningMark::anchorOver(anchor, cursor.x, cursor.left, cursor.width, metrics.left, metrics.width);
    includeGlyph(metrics, x, raiseBy);
  }
};

// The measured union can only grow after a complete, ligature-resolved glyph.
// Compile out the limit branch for callers that need the complete dimensions.
template <bool checkWidth>
bool measureText(const EpdFontFamily& family, const char* string, const EpdFontFamily::Style style, TextBounds& bounds,
                 const int maxWidth) {
  TextCursor cursor;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    if (utf8IsVariationSelector(cp)) continue;

    const bool isCombining = utf8IsCombiningMark(cp);
    if (!isCombining) cp = family.applyLigatures(cp, string, style);

    GlyphMetrics metrics;
    if (!measureDimensionGlyph(family, cp, style, isCombining, bounds.measureVertical, metrics)) {
      if (!isCombining) cursor.clearBase();
      continue;
    }

    if (isCombining) {
      bounds.includeMark(cp, metrics, cursor);
    } else {
      cursor.advance(family, cp, style);
      bounds.includeGlyph(metrics, cursor.x, 0);
      cursor.setBase(cp, metrics);
    }
    if (checkWidth && bounds.maxX - bounds.minX > maxWidth) return false;
  }
  return true;
}

}  // namespace

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font variant bits; decoration and positioning bits do not affect font selection.
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style, int* inkMinY,
                                      int* inkMaxY) const {
  const bool measureInk = inkMinY || inkMaxY;
  const bool measureVertical = h || measureInk;
  TextBounds bounds(measureVertical, measureInk);
  measureText<false>(*this, string, style, bounds, 0);

  *w = bounds.maxX - bounds.minX;
  if (h) *h = bounds.maxY - bounds.minY;
  if (inkMinY) *inkMinY = bounds.inkMinY;
  if (inkMaxY) *inkMaxY = bounds.inkMaxY;
}

bool EpdFontFamily::textFitsWidth(const char* string, const int maxWidth, const Style style) const {
  if (maxWidth < 0) return false;
  TextBounds bounds(false, false);
  return measureText<true>(*this, string, style, bounds, maxWidth);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

EpdFontFamily::GlyphData EpdFontFamily::findGlyphData(const uint32_t cp, const Style style) const {
  const EpdFont* font = getFont(style);
  if (const EpdGlyph* glyph = font->findGlyph(cp)) {
    return {font->data, glyph};
  }

  if (font != regular) {
    if (const EpdGlyph* glyph = regular->findGlyph(cp)) {
      return {regular->data, glyph};
    }
  }

  if (fallback) {
    if (const EpdGlyph* glyph = fallback->findGlyph(cp)) {
      return {fallback->data, glyph};
    }
  }
  return {nullptr, nullptr};
}

EpdFontFamily::GlyphData EpdFontFamily::getGlyphData(const uint32_t cp, const Style style) const {
  if (const GlyphData glyphData = findGlyphData(cp, style); glyphData.glyph) {
    return glyphData;
  }

  if (cp != REPLACEMENT_GLYPH) {
    return getGlyphData(REPLACEMENT_GLYPH, style);
  }
  return {nullptr, nullptr};
}

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  return getGlyphData(cp, style).glyph;
}

uint32_t EpdFontFamily::getFallbackCodepoint(const uint32_t cp, const Style style) const {
  const EpdGlyph* glyph = findGlyphData(cp, style).glyph;
  return glyph ? cp : normalizeMissingCodepoint(*this, cp, style, glyph);
}

bool EpdFontFamily::hasCodepoint(const uint32_t cp, const Style style) const {
  return getFont(style)->hasCodepoint(cp) || (fallback && fallback->hasCodepoint(cp));
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
