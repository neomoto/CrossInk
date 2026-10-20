#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <SdCardFont.h>
#include <TouchReaderPreviewModel.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>

#include <array>

namespace {
// Deterministic 2-bit glyphs with negative bearings and descenders. Both the
// built-in and real .cpfont loaders use these bytes, including RTL/CJK/marks.
struct RasterFont {
  std::vector<EpdUnicodeInterval> intervals;
  std::vector<EpdGlyph> glyphs;
  std::vector<uint8_t> pixels;
  EpdFontData data{};
  EpdFont font{&data};
  explicit RasterFont(int size) {
    for (auto [first, last] : {std::pair{32u, 127u},
                               {0xB7u, 0xB7u},
                               {0x300u, 0x36Fu},
                               {0x590u, 0x6FFu},
                               {0x4E00u, 0x4E20u},
                               {0xFB50u, 0xFEFFu},
                               {0xFFFDu, 0xFFFDu}}) {
      intervals.push_back({first, last, static_cast<uint32_t>(glyphs.size())});
      for (unsigned cp = first; cp <= last; ++cp) {
        EpdGlyph g{};
        g.width = size;
        g.height = size + 4;
        g.advanceX = size * 16;
        g.left = -3;
        g.top = size + 1;
        if (cp >= 0x300 && cp <= 0x36f) {
          g.advanceX = 0;
          g.top = size + 5;
        }
        g.dataOffset = pixels.size();
        g.dataLength = (g.width * g.height + 3) / 4;
        glyphs.push_back(g);
        for (int b = 0; b < g.dataLength; ++b) pixels.push_back(b % 2 ? 0x1B : 0xE4);
      }
    }
    data.bitmap = pixels.data();
    data.glyph = glyphs.data();
    data.intervals = intervals.data();
    data.intervalCount = intervals.size();
    data.advanceY = size + 6;
    data.ascender = size + 1;
    data.descender = -3;
    data.is2Bit = true;
  }
  template <class T>
  static void append(std::vector<uint8_t>& bytes, const T& value) {
    auto p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
  }
  std::vector<uint8_t> file() const {
    std::vector<uint8_t> bytes(64);
    memcpy(bytes.data(), "CPFONT\0\0", 8);
    auto put16 = [&](int o, uint16_t v) { memcpy(bytes.data() + o, &v, 2); };
    auto put32 = [&](int o, uint32_t v) { memcpy(bytes.data() + o, &v, 4); };
    put16(8, CPFONT_VERSION);
    put16(10, 1);
    bytes[12] = 1;
    put32(36, intervals.size());
    put32(40, glyphs.size());
    bytes[44] = data.advanceY;
    put16(45, data.ascender);
    put16(47, uint16_t(data.descender));
    put32(56, 64);
    for (auto& i : intervals) append(bytes, i);
    for (auto& g : glyphs) append(bytes, g);
    bytes.insert(bytes.end(), pixels.begin(), pixels.end());
    return bytes;
  }
};

TEST(EpubTextGrayscaleTest, RealTextRasterMatchesFullAndStripTargets) {
  for (bool sd : {false, true})
    for (int size : {12, 20})
      for (int orientation = 0; orientation < 4; ++orientation) {
        SCOPED_TRACE(testing::Message() << sd << ' ' << size << ' ' << orientation);
        fakeheap::reset(true);
        Storage.reset();
        RasterFont fixture(size);
        SdCardFont sdFont;
        HalDisplay display(orientation % 2 ? 800 : 792, 481);
        GfxRenderer renderer(display);
        renderer.begin();
        if (sd) {
          Storage.put("font.cpfont", fixture.file());
          ASSERT_TRUE(sdFont.load("font.cpfont"));
          renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
          renderer.registerSdCardFont(1, &sdFont);
        } else
          renderer.insertFont(1, EpdFontFamily(&fixture.font));
        renderer.setOrientation(GfxRenderer::Orientation(orientation));
        FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
        renderer.setFontCacheManager(&cache);
        const std::vector<std::string> words = {"Abc", "e\u0301", "שלום", "سلام", "一", "\U0001F642", " "};
        const std::vector<int16_t> offsets = {0, 65, 120, 190, 260, 290, 330};
        const std::vector<EpdFontFamily::Style> styles = {
            EpdFontFamily::REGULAR, EpdFontFamily::BOLD,    EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC,
            EpdFontFamily::REGULAR, EpdFontFamily::REGULAR, EpdFontFamily::REGULAR};
        const auto bw = display.bw;
        for (int feature = 0; feature < 5; ++feature) {
          auto variants = styles;
          BlockStyle blockStyle;
          blockStyle.isRtl = feature == 4;
          for (auto& style : variants)
            style = EpdFontFamily::Style(style | (feature == 0 ? EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH
                                                  : feature == 1 ? EpdFontFamily::SMALL_CAPS
                                                  : feature == 2 ? EpdFontFamily::SUP
                                                  : feature == 3 ? EpdFontFamily::SUB
                                                                 : 0));
          TextBlock line(words, offsets, variants,
                         feature == 1 ? std::vector<uint8_t>(words.size(), 1) : std::vector<uint8_t>{},
                         feature == 1 ? std::vector<uint16_t>(words.size(), 12) : std::vector<uint16_t>{},
                         std::vector<uint16_t>(words.size(), 6),
                         std::vector<uint8_t>(words.size(), TextBlock::WORD_FLAG_BACKGROUND_BLACK), {}, blockStyle,
                         feature == 4 ? std::vector<std::string>(words.size(), "Ab") : std::vector<std::string>{});
          ASSERT_TRUE(line.valid());
          auto draw = [&] {
            for (int y : {-8, 65, 79, 145, 159, 230, 310, 390, 470}) line.render(renderer, 1, 7, y, true);
          };
          {
            auto scan = cache.createPrewarmScope();
            draw();
            scan.endScanAndPrewarm();
          }
          EXPECT_EQ(display.bw, bw);
          for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
            renderer.setRenderMode(mode);
            std::vector<uint8_t> full(display.bw.size()), strips(display.bw.size());
            renderer.beginStripTarget(full.data(), 0, display.height);
            renderer.clearScreen(mode == GfxRenderer::BW ? 255 : 0);
            draw();
            renderer.endStripTarget();
            for (int y = 0; y < display.height; y += 80) {
              int rows = std::min(80, display.height - y);
              renderer.beginStripTarget(strips.data() + size_t(y) * display.stride, y, rows);
              renderer.clearScreen(mode == GfxRenderer::BW ? 255 : 0);
              draw();
              renderer.endStripTarget();
            }
            EXPECT_EQ(full, strips) << "feature=" << feature << " mode=" << mode;
            EXPECT_TRUE(std::any_of(full.begin(), full.end(),
                                    [&](auto b) { return b != (mode == GfxRenderer::BW ? 255 : 0); }));
            EXPECT_EQ(display.bw, bw);
            EXPECT_EQ(renderer.getWriteTarget(), display.bw.data());
          }
          renderer.setRenderMode(GfxRenderer::BW);
        }
        renderer.setFontCacheManager(nullptr);
        renderer.removeFont(1);
      }
}
}  // namespace

TEST(EpubTextRaster, VariationSelectorsDoNotDrawOrAdvance) {
  for (const bool sd : {false, true}) {
    SCOPED_TRACE(testing::Message() << "sd=" << sd);
    fakeheap::reset(true);
    Storage.reset();
    RasterFont fixture(12);
    SdCardFont sdFont;
    HalDisplay display;
    GfxRenderer renderer(display);
    renderer.begin();
    if (sd) {
      Storage.put("font.cpfont", fixture.file());
      ASSERT_TRUE(sdFont.load("font.cpfont"));
      renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
      renderer.registerSdCardFont(1, &sdFont);
    } else {
      renderer.insertFont(1, EpdFontFamily(&fixture.font));
    }

    const auto render = [&renderer, &display](const char* text) {
      renderer.clearScreen();
      renderer.drawText(1, 25, 40, text);
      return display.bw;
    };

    const auto base = render("!");
    const int baseAdvance = renderer.getTextAdvanceX(1, "!", EpdFontFamily::REGULAR);
    for (const char* text : {"!\xE1\xA0\x8B", "!\xEF\xB8\x8E", "!\xEF\xB8\x8F"}) {  // U+180B/U+FE0E/U+FE0F
      EXPECT_EQ(renderer.getTextAdvanceX(1, text, EpdFontFamily::REGULAR), baseAdvance);
      EXPECT_EQ(render(text), base);
    }
    renderer.removeFont(1);
  }
}

TEST(AbsoluteImageRaster, TextMatchesBlackWhiteInBothPlanesAndCancellationResetsMode) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(GfxRenderer::Orientation(orientation));
    renderer.clearScreen();
    renderer.drawText(1, 25, 40, "Book cover");
    const auto expected = display.bw;
    ASSERT_TRUE(renderer.displayAbsoluteGrayscaleBase());
    for (auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.clearScreen();
      renderer.setRenderMode(mode);
      renderer.drawText(1, 25, 40, "Book cover");
      EXPECT_EQ(display.bw, expected);
    }
    renderer.setRenderMode(GfxRenderer::BW);
    EXPECT_FALSE(renderer.grayPlanesAreAbsolute());
  }
  EXPECT_EQ(display.canceled, 4);
  display.absoluteSupported = false;
  EXPECT_FALSE(renderer.supportsAbsoluteGrayscale());
  EXPECT_FALSE(renderer.displayAbsoluteGrayscaleBase());
  EXPECT_FALSE(renderer.grayPlanesAreAbsolute());
}

TEST(AbsoluteImageRaster, BitmapPlanesPreserveFourTonesAndWhiteMargins) {
  fakeheap::reset(true);
  Storage.reset();
  // Two identical bottom-up rows of black/dark/light/white, padded to four bytes.
  auto data = std::make_shared<HostFileData>();
  data->bytes.resize(78, 0);
  auto put16 = [&](int offset, uint16_t value) { memcpy(data->bytes.data() + offset, &value, 2); };
  auto put32 = [&](int offset, uint32_t value) { memcpy(data->bytes.data() + offset, &value, 4); };
  put16(0, 0x4d42);
  put32(2, 78);
  put32(10, 70);
  put32(14, 40);
  put32(18, 4);
  put32(22, 2);
  put16(26, 1);
  put16(28, 2);
  put32(34, 8);
  put32(46, 4);
  for (int level = 0; level < 4; ++level)
    for (int channel = 0; channel < 3; ++channel) data->bytes[54 + level * 4 + channel] = level * 85;
  data->bytes[70] = data->bytes[74] = 0x1b;
  HalFile file(data);
  Bitmap bitmap(file);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  HalDisplay display(8, 4);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  ASSERT_TRUE(renderer.displayAbsoluteGrayscaleBase());
  int plane = 0;
  for (auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
    renderer.clearScreen();
    renderer.setRenderMode(mode);
    ASSERT_TRUE(renderer.drawBitmap(bitmap, 0, 3, 8, 4));
    // The first file row lies below the screen. The second must still be read.
    EXPECT_EQ(display.bw[0], 0xff);
    EXPECT_EQ(display.bw[1], 0xff);
    EXPECT_EQ(display.bw[2], 0xff);
    EXPECT_EQ(display.bw[3], plane++ == 0 ? 0x5f : 0x3f);
  }
  // A truncated second pass must be reported, allowing the caller to cancel it.
  ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
  data->readFailAt = 74;
  EXPECT_FALSE(renderer.drawBitmap(bitmap, 0, 0, 8, 4));
  renderer.setRenderMode(GfxRenderer::BW);
  EXPECT_EQ(display.canceled, 1);
  file.close();
}

// A font switch begins with no resident glyphs. A wide replacement glyph makes
// the cold scan fit fewer words than the final, correctly measured preview.
TEST(EpubTextGrayscaleTest, ColdSdSamplePreviewMatchesFullyLoadedFont) {
  for (int size : {12, 20}) {
    for (int width : {160, 280}) {
      for (bool focus : {false, true}) {
        for (bool guide : {false, true}) {
          SCOPED_TRACE(testing::Message() << size << " width=" << width << " focus=" << focus << " guide=" << guide);
          fakeheap::reset(false);
          Storage.reset();
          RasterFont fixture(size);
          fixture.glyphs.back().advanceX = size * 16 * 4;
          SdCardFont sdFont;
          Storage.put("preview.cpfont", fixture.file());
          ASSERT_TRUE(sdFont.load("preview.cpfont"));
          HalDisplay display;
          GfxRenderer renderer(display);
          renderer.begin();
          renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
          renderer.registerSdCardFont(1, &sdFont);
          renderer.insertFont(2, EpdFontFamily(&fixture.font));
          FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
          renderer.setFontCacheManager(&cache);
          SampleReaderPreviewModel model;
          ASSERT_TRUE(model.captureParagraph(READER_PREVIEW_PARAGRAPH));
          auto draw = [&](int font) {
            model.renderText(renderer, font, 10, 10, width, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), focus,
                             guide, true, 85);
          };
          renderer.clearScreen();
          draw(2);
          const auto expected = display.bw;
          renderer.clearScreen();
          const auto blank = display.bw;
          auto scope = cache.createPrewarmScope();
          draw(1);
          EXPECT_EQ(display.bw, blank);  // Scanning must not paint the display.
          ASSERT_TRUE(scope.endScanAndPrewarm());
          draw(1);
          EXPECT_NE(display.bw, blank);
          EXPECT_TRUE(display.bw == expected);
        }
      }
    }
  }
}

namespace {
// Each face has different source pixels and may have a different packed format.
// The oracle below generates pixels independently rather than asking the family
// to choose a glyph or calling the production bitmap/scaling helpers.
struct ResolutionRasterFace {
  std::vector<uint32_t> codepoints;
  std::vector<EpdUnicodeInterval> intervals;
  std::vector<EpdGlyph> glyphs;
  std::vector<uint8_t> pixels;
  EpdFontData data{};
  EpdFont font{&data};
  int seed;

  ResolutionRasterFace(std::initializer_list<uint32_t> cps, const bool twoBit, const int patternSeed)
      : codepoints(cps), seed(patternSeed) {
    for (const uint32_t cp : codepoints) {
      intervals.push_back({cp, cp, static_cast<uint32_t>(glyphs.size())});
      EpdGlyph glyph{5, 5, 128, -1, 4, static_cast<uint16_t>(twoBit ? 7 : 4), static_cast<uint32_t>(pixels.size())};
      pixels.resize(pixels.size() + glyph.dataLength, 0);
      for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x) {
          const int pos = y * 5 + x;
          const unsigned raw = source(cp, x, y, twoBit);
          const int shift = twoBit ? (3 - pos % 4) * 2 : 7 - pos % 8;
          pixels[glyph.dataOffset + pos / (twoBit ? 4 : 8)] |= raw << shift;
        }
      }
      glyphs.push_back(glyph);
    }
    data.bitmap = pixels.data();
    data.glyph = glyphs.data();
    data.intervals = intervals.data();
    data.intervalCount = intervals.size();
    data.advanceY = 10;
    data.ascender = 8;
    data.is2Bit = twoBit;
  }

  unsigned source(const uint32_t cp, const int x, const int y, const bool twoBit) const {
    return (cp + seed + x * 3 + y * 2) % (twoBit ? 4 : 2);
  }
};

enum class ResolutionScale { Full, Half, SmallCap };

int resolutionScaleValue(const ResolutionScale scale, const int full, const int half, const int smallCap) {
  if (scale == ResolutionScale::Full) return full;
  return scale == ResolutionScale::Half ? half : smallCap;
}

std::pair<int, int> resolutionSourceRange(const int destination, const ResolutionScale scale) {
  if (scale == ResolutionScale::Full) return {destination, destination + 1};
  if (scale == ResolutionScale::Half) return {destination * 2, std::min(5, destination * 2 + 2)};
  return {destination * 4 / 3, std::min(5, ((destination + 1) * 4 + 2) / 3)};
}

std::pair<unsigned, unsigned> resolutionCoverage(const ResolutionRasterFace& face, const uint32_t cp, const int x,
                                                 const int y, const ResolutionScale scale) {
  const auto [sx, ex] = resolutionSourceRange(x, scale);
  const auto [sy, ey] = resolutionSourceRange(y, scale);
  unsigned maximum = 0, total = 0;
  for (int by = sy; by < ey; ++by) {
    for (int bx = sx; bx < ex; ++bx) {
      const unsigned raw = face.source(cp, bx, by, face.data.is2Bit);
      maximum = std::max(maximum, raw);
      total += raw;
    }
  }
  return {maximum, total};
}

bool resolutionHasInk(const bool twoBit, const ResolutionScale scale, const GfxRenderer::RenderMode mode,
                      const unsigned maximum, const unsigned total) {
  if (!twoBit) return maximum != 0;
  if (scale == ResolutionScale::Half) return maximum >= 2 || total >= 2;
  if (mode == GfxRenderer::BW) return maximum != 0;
  if (mode == GfxRenderer::GRAYSCALE_LSB) return maximum == 2;
  return maximum == 1 || maximum == 2;
}

void paintResolutionPixel(std::vector<uint8_t>& expected, const HalDisplay& display, const int orientation, const int x,
                          const int y, const bool black) {
  int px = x, py = y;
  switch (orientation) {
    case 0:
      px = y;
      py = display.height - 1 - x;
      break;
    case 1:
      px = display.width - 1 - x;
      py = display.height - 1 - y;
      break;
    case 2:
      px = display.width - 1 - y;
      py = x;
      break;
    default:
      break;
  }
  if (px < 0 || py < 0 || px >= display.width || py >= display.height) return;
  auto& byte = expected[py * display.stride + px / 8];
  const uint8_t mask = 0x80 >> (px % 8);
  if (black)
    byte &= static_cast<uint8_t>(~mask);
  else
    byte |= mask;
}

void paintResolutionGlyph(std::vector<uint8_t>& expected, const HalDisplay& display, const int orientation,
                          const ResolutionRasterFace& face, const uint32_t cp, const int cursorX, const int cursorY,
                          const bool rotated, const ResolutionScale scale, const GfxRenderer::RenderMode mode,
                          const bool state) {
  const int extent = resolutionScaleValue(scale, 5, 3, 4);
  const int left = resolutionScaleValue(scale, -1, 0, 0);
  const int top = resolutionScaleValue(scale, 4, 2, 3);
  const bool binary = !face.data.is2Bit || scale == ResolutionScale::Half;
  const bool black = state && (binary || mode == GfxRenderer::BW);
  for (int y = 0; y < extent; ++y) {
    for (int x = 0; x < extent; ++x) {
      const auto [maximum, total] = resolutionCoverage(face, cp, x, y, scale);
      if (!resolutionHasInk(face.data.is2Bit, scale, mode, maximum, total)) continue;
      const int px = rotated ? cursorX + 8 - top + y : cursorX + left + x;
      const int py = rotated ? cursorY - left - x : cursorY - top + y;
      paintResolutionPixel(expected, display, orientation, px, py, black);
    }
  }
}

TEST(EpubTextRaster, MatchedStyleRegularFallbackAliasAndMissingMarkHaveIndependentPixels) {
  for (const bool twoBit : {false, true}) {
    ResolutionRasterFace regular({'A', 'M', 'R', 0x2018, 0xFFFD}, twoBit, 1);
    ResolutionRasterFace bold({'B', 'M'}, !twoBit, 2);
    ResolutionRasterFace fallback({'F', 'Q', 0x302}, twoBit, 3);
    HalDisplay display(64, 64);
    GfxRenderer renderer(display);
    renderer.begin();
    renderer.insertFont(1, EpdFontFamily(&regular.font, &bold.font, nullptr, nullptr, &fallback.font));
    for (int orientation = 0; orientation < 4; ++orientation) {
      renderer.setOrientation(GfxRenderer::Orientation(orientation));
      for (const bool rotated : {false, true}) {
        for (const bool black : {false, true}) {
          for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
            SCOPED_TRACE(testing::Message()
                         << twoBit << ' ' << orientation << ' ' << rotated << ' ' << black << ' ' << mode);
            renderer.setRenderMode(mode);
            renderer.clearScreen(0xA5);
            auto expected = display.bw;
            const std::array<std::pair<const ResolutionRasterFace*, uint32_t>, 7> bases = {{{&bold, 'B'},
                                                                                            {&regular, 'A'},
                                                                                            {&regular, 'R'},
                                                                                            {&fallback, 'F'},
                                                                                            {&regular, 0x2018},
                                                                                            {&fallback, 'Q'},
                                                                                            {&bold, 'B'}}};
            for (int i = 0; i < 7; ++i) {
              paintResolutionGlyph(expected, display, orientation, *bases[i].first, bases[i].second,
                                   rotated ? 10 : 6 + i * 8, rotated ? 58 - i * 8 : 24, rotated, ResolutionScale::Full,
                                   mode, black);
            }
            for (auto [face, cp] : {std::pair{&fallback, 0x302u}, std::pair{&regular, 0xFFFDu}}) {
              paintResolutionGlyph(expected, display, orientation, *face, cp, rotated ? 10 : 46, rotated ? 18 : 24,
                                   rotated, ResolutionScale::Full, mode, black);
            }
            if (rotated)
              renderer.drawTextRotated90CW(1, 10, 58, "BARF\u02BBQ\u0302\u0303B", black, EpdFontFamily::BOLD);
            else
              renderer.drawText(1, 6, 16, "BARF\u02BBQ\u0302\u0303B", black, EpdFontFamily::BOLD);
            EXPECT_EQ(display.bw, expected);
          }
        }
      }
    }
  }
}

TEST(EpubTextRaster, ScaledFallbackRetainsItsOwnBitmapFormatAndOwner) {
  for (const bool twoBit : {false, true}) {
    ResolutionRasterFace regular({'A', 'M', 'R'}, twoBit, 1);
    ResolutionRasterFace bold({'M'}, !twoBit, 2);
    ResolutionRasterFace fallback({'F'}, twoBit, 3);
    HalDisplay display(64, 64);
    GfxRenderer renderer(display);
    renderer.begin();
    renderer.insertFont(1, EpdFontFamily(&regular.font, &bold.font, nullptr, nullptr, &fallback.font));
    for (int orientation = 0; orientation < 4; ++orientation) {
      renderer.setOrientation(GfxRenderer::Orientation(orientation));
      for (const auto overlay : {EpdFontFamily::SUP, EpdFontFamily::SUB, EpdFontFamily::SMALL_CAPS}) {
        const bool small = overlay == EpdFontFamily::SMALL_CAPS;
        for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
          SCOPED_TRACE(testing::Message() << twoBit << ' ' << orientation << ' ' << overlay << ' ' << mode);
          renderer.setRenderMode(mode);
          renderer.clearScreen(0xA5);
          auto expected = display.bw;
          int x = 10;
          for (auto [face, cp] : {std::pair{&regular, 'A'}, std::pair{&regular, 'R'}, std::pair{&fallback, 'F'}}) {
            paintResolutionGlyph(expected, display, orientation, *face, cp, x, 24, false,
                                 small ? ResolutionScale::SmallCap : ResolutionScale::Half, mode, true);
            x += small ? 6 : 4;
          }
          renderer.drawText(1, 10, 16, small ? "arf" : "ARF", true,
                            EpdFontFamily::Style(EpdFontFamily::BOLD | overlay));
          EXPECT_EQ(display.bw, expected);
        }
      }
    }
  }
}

TEST(EpubTextRaster, MissingSpaceAndMissingMarkPreservePreviousAdvanceWithoutSyntheticMarkInk) {
  ResolutionRasterFace face({'A', 'B', 'M'}, true, 4);
  HalDisplay display(64, 64);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&face.font));
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(GfxRenderer::Orientation(orientation));
    for (const bool rotated : {false, true}) {
      for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
        renderer.setRenderMode(mode);
        renderer.clearScreen(0xA5);
        auto expected = display.bw;
        paintResolutionGlyph(expected, display, orientation, face, 'A', 10, rotated ? 54 : 24, rotated,
                             ResolutionScale::Full, mode, true);
        paintResolutionGlyph(expected, display, orientation, face, 'B', rotated ? 10 : 18, rotated ? 46 : 24, rotated,
                             ResolutionScale::Full, mode, true);
        if (rotated)
          renderer.drawTextRotated90CW(1, 10, 54, "A\u2007\u0303B", true);
        else
          renderer.drawText(1, 10, 16, "A\u2007\u0303B", true);
        EXPECT_EQ(display.bw, expected);
      }
    }
  }
}

#if CROSSINK_SCALABLE_FONTS
struct EvictingResolutionFont {
  ResolutionRasterFace source{{'A', 'B', 0x301, 0x2018, 0xFFFD}, true, 5};
  EpdGlyph slot{};
  std::array<uint8_t, 7> bitmap{};
  EpdFontData data{};
  EpdFont font{&data};

  EvictingResolutionFont() {
    data.ascender = 8;
    data.advanceY = 10;
    data.is2Bit = true;
    data.glyphMissCtx = this;
    data.dynamicGlyphHandler = [](void* ctx, const uint32_t cp) -> const EpdGlyph* {
      auto& self = *static_cast<EvictingResolutionFont*>(ctx);
      self.slot = {};
      self.bitmap.fill(0);
      const auto it = std::find(self.source.codepoints.begin(), self.source.codepoints.end(), cp);
      if (it == self.source.codepoints.end()) return nullptr;
      const auto index = static_cast<size_t>(it - self.source.codepoints.begin());
      self.slot = self.source.glyphs[index];
      std::copy_n(self.source.pixels.data() + self.slot.dataOffset, self.bitmap.size(), self.bitmap.data());
      self.slot.dataOffset = cp;
      return &self.slot;
    };
    data.kerningHandler = [](void* ctx, uint32_t, const uint32_t right) -> int8_t {
      auto& self = *static_cast<EvictingResolutionFont*>(ctx);
      // Model a callback faulting other glyphs into the same ring and pixels.
      self.slot = {1, 1, 16, 9, 9, 1, 0};
      self.bitmap.fill(0);
      return right == 0xFFFD ? 0 : right == 0x2018 ? -32 : -16;
    };
    data.bitmapHandler = [](void* ctx, const EpdGlyph* glyph) -> const uint8_t* {
      auto& self = *static_cast<EvictingResolutionFont*>(ctx);
      if (glyph != &self.slot || self.slot.dataOffset == 0) return nullptr;
      // A bitmap request may also reuse the metrics slot. Placement/advance
      // must already have been consumed, while the returned pixels stay valid.
      self.slot = {1, 1, 16, 9, 9, 1, 0};
      return self.bitmap.data();
    };
  }
};

int resolutionKernedStep(const ResolutionScale scale, const uint32_t right) {
  const int advance = resolutionScaleValue(scale, 8, 4, 6);
  if (right == 0xFFFD) return advance;
  return advance - (right == 0x2018 ? 2 : 1);
}

void checkEvictingResolutionRaster(const int orientation, const bool rotated, const EpdFontFamily::Style overlay,
                                   const GfxRenderer::RenderMode mode) {
  SCOPED_TRACE(testing::Message() << orientation << ' ' << rotated << ' ' << overlay << ' ' << mode);
  EvictingResolutionFont fixture;
  HalDisplay display(64, 64);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  renderer.setOrientation(GfxRenderer::Orientation(orientation));
  renderer.setRenderMode(mode);
  renderer.clearScreen(0xA5);
  auto expected = display.bw;
  const bool small = overlay == EpdFontFamily::SMALL_CAPS;
  const auto scale = overlay == EpdFontFamily::SUP ? ResolutionScale::Half
                     : small                       ? ResolutionScale::SmallCap
                                                   : ResolutionScale::Full;
  const std::vector<uint32_t> cps =
      small ? std::vector<uint32_t>{'A', 'B', 0xFFFD, 'B'} : std::vector<uint32_t>{'A', 'B', 0x2018, 0xFFFD, 'B'};
  int offset = 0;
  for (size_t i = 0; i < cps.size(); ++i) {
    if (i != 0) offset += resolutionKernedStep(scale, cps[i]);
    paintResolutionGlyph(expected, display, orientation, fixture.source, cps[i], rotated ? 10 : 10 + offset,
                         rotated ? 54 - offset : 24, rotated, scale, mode, true);
  }
  if (!small) {
    // Combining overlays keep the unscaled base metrics even in SUP text.
    paintResolutionGlyph(expected, display, orientation, fixture.source, 0x301,
                         rotated ? 10 : 10 + 2 * resolutionScaleValue(scale, 7, 3, 5) - 1,
                         rotated ? 55 - 2 * resolutionScaleValue(scale, 7, 3, 5) : 24, rotated, ResolutionScale::Full,
                         mode, true);
  }
  if (rotated)
    renderer.drawTextRotated90CW(1, 10, 54, "AB\u02BB\u0301ZB", true);
  else
    renderer.drawText(1, 10, 16, small ? "abzb" : "AB\u02BB\u0301ZB", true, overlay);
  EXPECT_EQ(display.bw, expected);
}

TEST(EpubTextRaster, DynamicKerningAndBitmapEvictionsPreserveIdentityPlacementAndFollowingAdvance) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (const bool rotated : {false, true}) {
      for (const auto overlay : {EpdFontFamily::REGULAR, EpdFontFamily::SUP, EpdFontFamily::SMALL_CAPS}) {
        if (rotated && overlay != EpdFontFamily::REGULAR) continue;
        for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB})
          checkEvictingResolutionRaster(orientation, rotated, overlay, mode);
      }
    }
  }
}
#endif
}  // namespace
