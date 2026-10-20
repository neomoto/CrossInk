#include "TextBlock.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>

#include "../../../ScalableFont/ScalableFontSizing.h"

namespace {

constexpr uint16_t MAX_WORDS_PER_TEXT_BLOCK = 512;

uint16_t measureBackgroundWidth(const GfxRenderer& renderer, const int fontId, const char* word,
                                const EpdFontFamily::Style style) {
  if (word[0] == ' ' && word[1] == '\0') {
    return renderer.getSpaceWidth(fontId, style);
  }
  return static_cast<uint16_t>(std::max(0, renderer.getTextAdvanceX(fontId, word, style)));
}

bool isWhitespaceOnlyBackgroundToken(const char* word) {
  if (!word || *word == '\0') {
    return false;
  }

  for (size_t i = 0; word[i] != '\0';) {
    const auto c = static_cast<uint8_t>(word[i]);
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      ++i;
      continue;
    }
    if (c == 0xC2 && word[i + 1] != '\0' && static_cast<uint8_t>(word[i + 1]) == 0xA0) {
      i += 2;
      continue;
    }
    if (c == 0xE2 && word[i + 1] != '\0' && word[i + 2] != '\0' && static_cast<uint8_t>(word[i + 1]) == 0x80 &&
        static_cast<uint8_t>(word[i + 2]) == 0xAF) {
      i += 3;
      continue;
    }
    return false;
  }

  return true;
}

bool hasSyntheticIndentPrefix(const char* word, const uint16_t len) {
  return len >= 3 && static_cast<uint8_t>(word[0]) == 0xE2 && static_cast<uint8_t>(word[1]) == 0x80 &&
         static_cast<uint8_t>(word[2]) == 0x83;
}

}  // namespace

size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasFocus, const bool hasGuideDots,
                            const bool hasWordFlags, const bool hasWordSpaces, const uint16_t textBytes) {
  // 16-bit arrays first so direct loads stay aligned on RISC-V, then byte arrays, then text.
  size_t size = static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t));
  if (hasFocus) {
    size += static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(uint8_t));
  }
  if (hasGuideDots) {
    size += static_cast<size_t>(wordCount) * sizeof(uint16_t);
  }
  if (hasWordFlags) {
    size += static_cast<size_t>(wordCount) * sizeof(uint8_t);
  }
  if (hasWordSpaces) {
    size += wordSpacesBytes(wordCount);
  }
  return size + textBytes;
}

void TextBlock::bindArenaPointers() {
  uint8_t* base = arena.get();
  const size_t wc = numWords;
  textOffArr = reinterpret_cast<const uint16_t*>(base);
  xposArr = reinterpret_cast<const int16_t*>(base + wc * 2);
  size_t off = wc * 4;
  if (focusPresent) {
    focusRunOffsetArr = reinterpret_cast<const uint16_t*>(base + off);
    off += wc * 2;
  }
  if (guideDotsPresent) {
    guideDotXOffsetArr = reinterpret_cast<const uint16_t*>(base + off);
    off += wc * 2;
  }
  stylesArr = base + off;
  off += wc;
  if (focusPresent) {
    focusBoundaryArr = base + off;
    off += wc;
  }
  if (wordFlagsPresent) {
    wordFlagsArr = base + off;
    off += wc;
  }
  if (wordSpacesPresent) {
    wordSpacesArr = base + off;
    off += wordSpacesBytes(numWords);
  }
  textArr = reinterpret_cast<const char*>(base + off);
}

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusRunOffset, const std::vector<uint16_t>& guideDotXOffset,
                     const std::vector<uint8_t>& wordFlags, const std::vector<bool>& wordHasSpaceBefore,
                     const BlockStyle& blockStyle, std::vector<std::string> rubyTexts)
    : blockStyle(blockStyle), rubyTexts(std::move(rubyTexts)) {
  // A ruby-less line needs no per-word ruby vector. ParsedText passes one for
  // every extracted line once a book contains any ruby, so free all-empty
  // vectors before they stay resident with the page.
  if (!hasRuby()) {
    this->rubyTexts = std::vector<std::string>{};
  }

  const bool hasFocus = !focusBoundary.empty();
  const bool hasGuideDots = !guideDotXOffset.empty();
  const bool hasWordFlags = !wordFlags.empty();
  const bool hasWordSpaces = !wordHasSpaceBefore.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() || words.size() > MAX_WORDS_PER_TEXT_BLOCK ||
      (hasFocus && (words.size() != focusBoundary.size() || words.size() != focusRunOffset.size())) ||
      (!hasFocus && !focusRunOffset.empty()) || (hasGuideDots && words.size() != guideDotXOffset.size()) ||
      (hasWordFlags && words.size() != wordFlags.size()) ||
      (hasWordSpaces && words.size() != wordHasSpaceBefore.size()) ||
      (!this->rubyTexts.empty() && words.size() != this->rubyTexts.size())) {
    LOG_ERR("TXB",
            "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, runOffset=%u, "
            "dotX=%u, flags=%u, spaces=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(focusBoundary.size()),
            static_cast<uint32_t>(focusRunOffset.size()), static_cast<uint32_t>(guideDotXOffset.size()),
            static_cast<uint32_t>(wordFlags.size()), static_cast<uint32_t>(wordHasSpaceBefore.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  focusPresent = hasFocus;
  guideDotsPresent = hasGuideDots;
  wordFlagsPresent = hasWordFlags;
  wordSpacesPresent = hasWordSpaces;
  if (numWords == 0) {
    return;
  }

  size_t totalText = 0;
  for (const auto& word : words) {
    totalText += word.size() + 1;
  }
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    focusPresent = false;
    guideDotsPresent = false;
    wordFlagsPresent = false;
    wordSpacesPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size =
      arenaSize(numWords, focusPresent, guideDotsPresent, wordFlagsPresent, wordSpacesPresent, textBytes);
  arena = makeUniqueNoThrow<uint8_t[]>(size);
  if (!arena) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    focusPresent = false;
    guideDotsPresent = false;
    wordFlagsPresent = false;
    wordSpacesPresent = false;
    isValid = false;
    return;
  }
  bindArenaPointers();

  auto* textOff = const_cast<uint16_t*>(textOffArr);
  auto* xpos = const_cast<int16_t*>(xposArr);
  auto* styles = const_cast<uint8_t*>(stylesArr);
  auto* text = const_cast<char*>(textArr);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = wordXpos[i];
    styles[i] = static_cast<uint8_t>(wordStyles[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
  if (focusPresent) {
    auto* runOffset = const_cast<uint16_t*>(focusRunOffsetArr);
    auto* boundary = const_cast<uint8_t*>(focusBoundaryArr);
    for (uint16_t i = 0; i < numWords; i++) {
      runOffset[i] = focusRunOffset[i];
      boundary[i] = focusBoundary[i];
    }
  }
  if (guideDotsPresent) {
    auto* dotX = const_cast<uint16_t*>(guideDotXOffsetArr);
    for (uint16_t i = 0; i < numWords; i++) {
      dotX[i] = guideDotXOffset[i];
    }
  }
  if (wordFlagsPresent) {
    auto* flags = const_cast<uint8_t*>(wordFlagsArr);
    for (uint16_t i = 0; i < numWords; i++) {
      flags[i] = wordFlags[i];
    }
  }
  if (wordSpacesPresent) {
    auto* spaces = const_cast<uint8_t*>(wordSpacesArr);
    std::memset(spaces, 0, wordSpacesBytes(numWords));
    for (uint16_t i = 0; i < numWords; i++) {
      if (wordHasSpaceBefore[i]) {
        spaces[i / 8U] |= static_cast<uint8_t>(1U << (i % 8U));
      }
    }
  }
}

bool TextBlock::hasRuby() const {
  for (const auto& ruby : rubyTexts) {
    if (!ruby.empty()) return true;
  }
  return false;
}

int TextBlock::resolvedFontId(const GfxRenderer& renderer, const int fontId) const {
  return renderer.getFontIdForSize(fontId, blockStyle.fontSize);
}

void TextBlock::render(const GfxRenderer& renderer, int fontId, const int x, const int y,
                       const bool foregroundBlack) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid block");
    return;
  }

  fontId = resolvedFontId(renderer, fontId);
  const bool scanning = renderer.isFontCacheScanning();
  const int ascender = renderer.getFontAscenderSize(fontId);
  for (uint16_t i = 0; i < numWords; i++) {
    const char* word = wordText(i);
    const uint16_t wordLen = wordTextLen(i);
    const int wordX = wordXpos(i) + x;
    const EpdFontFamily::Style currentStyle = wordStyle(i);
    const uint8_t boundary = focusBoundary(i);
    const auto baseDir =
        static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(word, blockStyle.isRtl ? 1 : 0));

    if ((wordFlags(i) & WORD_FLAG_BACKGROUND_BLACK) != 0 && isWhitespaceOnlyBackgroundToken(word)) {
      const uint16_t backgroundWidth = measureBackgroundWidth(renderer, fontId, word, currentStyle);
      if (backgroundWidth > 0) {
        renderer.fillRect(wordX, y, backgroundWidth, renderer.getFontAscenderSize(fontId), true);
      }
    }

    int wordY = y + getRubyShift(ascender);
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    if (boundary > 0) {
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      char boldBuf[40];
      const size_t boldLen =
          std::min<size_t>({static_cast<size_t>(boundary), static_cast<size_t>(wordLen), sizeof(boldBuf) - 1});
      memcpy(boldBuf, word, boldLen);
      boldBuf[boldLen] = '\0';
      const int secondRunX = wordX + focusRunOffset(i);
      if (baseDir == BidiUtils::BidiBaseDir::RTL) {
        renderer.drawText(fontId, wordX, wordY, word + boldLen, foregroundBlack, currentStyle, baseDir);
        renderer.drawText(fontId, secondRunX, wordY, boldBuf, foregroundBlack, boldStyle, baseDir);
      } else {
        renderer.drawText(fontId, wordX, wordY, boldBuf, foregroundBlack, boldStyle, baseDir);
        renderer.drawText(fontId, secondRunX, wordY, word + boldLen, foregroundBlack, currentStyle, baseDir);
      }
    } else {
      renderer.drawText(fontId, wordX, wordY, word, foregroundBlack, currentStyle, baseDir);
    }

    if (i < rubyTexts.size() && !rubyTexts[i].empty() && (currentStyle & EpdFontFamily::RUBY_CONTINUE) == 0) {
      uint16_t groupWords = 1;
      while (i + groupWords < numWords && (wordStyle(i + groupWords) & EpdFontFamily::RUBY_CONTINUE) != 0) {
        ++groupWords;
      }
      int groupWidth = 0;
      for (uint16_t j = 0; j < groupWords; ++j) {
        groupWidth += renderer.getTextAdvanceX(fontId, wordText(i + j), wordStyle(i + j));
      }
      const int rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP);
      // ParsedText reserves any edge overhang in the line layout, so the ruby
      // can remain centered over its base text without screen-edge clamping.
      const int rubyX = wordX + (groupWidth - rubyWidth) / 2;
      renderer.drawText(fontId, rubyX, wordY - ascender, rubyTexts[i].c_str(), foregroundBlack, EpdFontFamily::SUP,
                        baseDir);
    }

    const uint16_t dotOffset = guideDotXOffset(i);
    if (dotOffset > 0) {
      renderer.drawText(fontId, wordX + dotOffset, wordY, "\xc2\xb7", foregroundBlack, EpdFontFamily::REGULAR, baseDir);
    }

    if (!scanning && (currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      int startX = wordX;
      int underlineWidth = renderer.getTextWidth(fontId, word, currentStyle, baseDir);
      const int underlineY = wordY + ascender + 2;

      if (hasSyntheticIndentPrefix(word, wordLen)) {
        const char* visiblePtr = word + 3;
        const int prefixWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        startX = wordX + prefixWidth;
        underlineWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle, baseDir);
      }

      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        underlineWidth = (underlineWidth + 1) / 2;
      }

      int underlineEndX = startX + underlineWidth;
      if (i + 1 < numWords) {
        const EpdFontFamily::Style nextStyle = wordStyle(i + 1);
        const bool nextSharesBaseline = (nextStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) ==
                                        (currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB));
        if ((nextStyle & EpdFontFamily::UNDERLINE) != 0 && nextSharesBaseline) {
          const int nextStartX = wordXpos(i + 1) + x;
          underlineEndX = std::max(underlineEndX, nextStartX);
          startX = std::min(startX, nextStartX);
        }
      }

      renderer.drawLine(startX, underlineY, underlineEndX, underlineY, 3, foregroundBlack);
    }

    if ((currentStyle & EpdFontFamily::STRIKETHROUGH) != 0) {
      int startX = wordX;
      int strikeWidth = renderer.getTextWidth(fontId, word, currentStyle, baseDir);
      const int strikeY = y + renderer.getFontAscenderSize(fontId) / 2 + 6;

      if (hasSyntheticIndentPrefix(word, wordLen)) {
        const char* visiblePtr = word + 3;
        const int prefixWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        startX = wordX + prefixWidth;
        strikeWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle, baseDir);
      }

      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        strikeWidth = (strikeWidth + 1) / 2;
      }

      int strikeEndX = startX + strikeWidth;
      if (i + 1 < numWords) {
        const EpdFontFamily::Style nextStyle = wordStyle(i + 1);
        const bool nextSharesBaseline = (nextStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) ==
                                        (currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB));
        if ((nextStyle & EpdFontFamily::STRIKETHROUGH) != 0 && nextSharesBaseline) {
          const int nextStartX = wordXpos(i + 1) + x;
          strikeEndX = std::max(strikeEndX, nextStartX);
          startX = std::min(startX, nextStartX);
        }
      }

      renderer.drawLine(startX, strikeY, strikeEndX, strikeY, 3, foregroundBlack);
    }
  }
}

bool TextBlock::serialize(HalFile& file) const {
  if (!isValid) {
    LOG_ERR("TXB", "Serialization failed: invalid block");
    return false;
  }

  if (!serialization::tryWritePodGroup(file, numWords, static_cast<uint8_t>(focusPresent),
                                       static_cast<uint8_t>(guideDotsPresent), static_cast<uint8_t>(wordFlagsPresent),
                                       static_cast<uint8_t>(wordSpacesPresent), textBytes)) {
    LOG_ERR("TXB", "Serialization failed: could not write block header");
    return false;
  }
  if (numWords > 0) {
    const size_t size =
        arenaSize(numWords, focusPresent, guideDotsPresent, wordFlagsPresent, wordSpacesPresent, textBytes);
    if (file.write(arena.get(), size) != static_cast<int>(size)) {
      LOG_ERR("TXB", "Serialization failed: arena write (%u bytes)", static_cast<uint32_t>(size));
      return false;
    }
  }

  if (!serializeRuby(file)) return false;

  return serialization::tryWritePodGroup(
      file, blockStyle.alignment, blockStyle.textAlignDefined, blockStyle.marginTop, blockStyle.marginBottom,
      blockStyle.marginLeft, blockStyle.marginRight, blockStyle.paddingTop, blockStyle.paddingBottom,
      blockStyle.paddingLeft, blockStyle.paddingRight, blockStyle.textIndent, blockStyle.textIndentDefined,
      blockStyle.isRtl, blockStyle.directionDefined, blockStyle.fontSize, blockStyle.lineHeight);
}

bool TextBlock::serializeRuby(HalFile& file) const {
  uint16_t rubyCount = 0;
  for (uint16_t i = 0; i < numWords && i < rubyTexts.size(); ++i) {
    if (!rubyTexts[i].empty()) ++rubyCount;
  }
  if (!serialization::tryWritePod(file, rubyCount)) return false;
  for (uint16_t i = 0; i < numWords && i < rubyTexts.size(); ++i) {
    if (rubyTexts[i].empty()) continue;
    if (!serialization::tryWritePod(file, i) || !serialization::tryWriteString(file, rubyTexts[i])) return false;
  }
  return true;
}

namespace {

bool validateTextBlockHeader(const uint16_t wordCount, const uint8_t hasFocus, const uint8_t hasGuideDots,
                             const uint8_t hasWordFlags, const uint8_t hasWordSpaces, const uint16_t textBytes) {
  if (wordCount > MAX_WORDS_PER_TEXT_BLOCK) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wordCount);
    return false;
  }
  if (hasFocus > 1 || hasGuideDots > 1 || hasWordFlags > 1 || hasWordSpaces > 1) {
    LOG_ERR("TXB", "Deserialization failed: invalid metadata flags");
    return false;
  }
  if ((wordCount == 0 && textBytes != 0) || (wordCount > 0 && textBytes < wordCount)) {
    LOG_ERR("TXB", "Deserialization failed: bad text size %u for %u words", textBytes, wordCount);
    return false;
  }
  return true;
}

std::unique_ptr<uint8_t[]> deserializeTextArena(HalFile& file, const size_t size) {
  const int remaining = file.available();
  if (remaining < 0 || static_cast<size_t>(remaining) < size) {
    LOG_ERR("TXB", "Deserialization failed: truncated arena (%u bytes needed, %d available)",
            static_cast<uint32_t>(size), remaining);
    return nullptr;
  }
  auto arena = makeUniqueNoThrow<uint8_t[]>(size);
  if (!arena) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    return nullptr;
  }
  if (file.read(arena.get(), size) != static_cast<int>(size)) {
    LOG_ERR("TXB", "Deserialization failed: arena read (%u bytes)", static_cast<uint32_t>(size));
    return nullptr;
  }
  return arena;
}

bool validateTextArena(const uint16_t* textOffsets, const char* text, const uint16_t wordCount,
                       const uint16_t textBytes) {
  if (textOffsets[0] != 0 || text[textBytes - 1] != '\0') {
    LOG_ERR("TXB", "Deserialization failed: corrupt text layout");
    return false;
  }
  for (uint16_t i = 1; i < wordCount; i++) {
    if (textOffsets[i] <= textOffsets[i - 1] || textOffsets[i] >= textBytes || text[textOffsets[i] - 1] != '\0') {
      LOG_ERR("TXB", "Deserialization failed: corrupt word offset %u", i);
      return false;
    }
  }
  return true;
}

bool deserializeRuby(HalFile& file, const uint16_t wordCount, std::vector<std::string>& rubyTexts) {
  uint16_t rubyCount = 0;
  if (!serialization::tryReadPod(file, rubyCount) || rubyCount > wordCount) {
    LOG_ERR("TXB", "Deserialization failed: invalid ruby count %u", rubyCount);
    return false;
  }
  if (rubyCount > 0) rubyTexts.resize(wordCount);
  for (uint16_t i = 0; i < rubyCount; ++i) {
    uint16_t wordIndex = 0;
    std::string ruby;
    if (!serialization::tryReadPod(file, wordIndex) || wordIndex >= wordCount ||
        !serialization::tryReadString(file, ruby)) {
      LOG_ERR("TXB", "Deserialization failed: invalid ruby annotation");
      return false;
    }
    rubyTexts[wordIndex] = std::move(ruby);
  }
  return true;
}

bool deserializeBlockStyle(HalFile& file, BlockStyle& style) {
  if (!serialization::tryReadPodGroup(file, style.alignment, style.textAlignDefined, style.marginTop,
                                      style.marginBottom, style.marginLeft, style.marginRight, style.paddingTop,
                                      style.paddingBottom, style.paddingLeft, style.paddingRight, style.textIndent,
                                      style.textIndentDefined, style.isRtl, style.directionDefined, style.fontSize,
                                      style.lineHeight) ||
      (style.fontSize != 0 &&
       (style.fontSize < ScalableContentMinPointSize || style.fontSize > ScalableContentMaxPointSize))) {
    LOG_ERR("TXB", "Deserialization failed: truncated block style metadata");
    return false;
  }
  return true;
}

}  // namespace

std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile& file) {
  uint16_t wc = 0;
  uint8_t hasFocus = 0;
  uint8_t hasGuideDots = 0;
  uint8_t hasWordFlags = 0;
  uint8_t hasWordSpaces = 0;
  uint16_t textBytes = 0;
  if (!serialization::tryReadPodGroup(file, wc, hasFocus, hasGuideDots, hasWordFlags, hasWordSpaces, textBytes)) {
    LOG_ERR("TXB", "Deserialization failed: could not read block header");
    return nullptr;
  }
  if (!validateTextBlockHeader(wc, hasFocus, hasGuideDots, hasWordFlags, hasWordSpaces, textBytes)) {
    return nullptr;
  }

  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    LOG_ERR("TXB", "Deserialization failed: could not allocate TextBlock");
    return nullptr;
  }
  block->numWords = wc;
  block->textBytes = textBytes;
  block->focusPresent = hasFocus != 0;
  block->guideDotsPresent = hasGuideDots != 0;
  block->wordFlagsPresent = hasWordFlags != 0;
  block->wordSpacesPresent = hasWordSpaces != 0;

  if (wc > 0) {
    const size_t size = arenaSize(wc, block->focusPresent, block->guideDotsPresent, block->wordFlagsPresent,
                                  block->wordSpacesPresent, textBytes);
    block->arena = deserializeTextArena(file, size);
    if (!block->arena) {
      return nullptr;
    }
    block->bindArenaPointers();
    if (!validateTextArena(block->textOffArr, block->textArr, wc, textBytes)) {
      return nullptr;
    }
  }

  if (!deserializeRuby(file, wc, block->rubyTexts) || !deserializeBlockStyle(file, block->blockStyle)) {
    return nullptr;
  }
  return block;
}
