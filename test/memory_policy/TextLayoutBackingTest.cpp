#include <Arduino.h>
#include <Epub/Page.h>
#include <Epub/ParsedText.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

// Hyphen dictionary availability is outside allocation policy. Exercise the
// production non-hyphenating layout, real bidi, TextBlock and serialization.
std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }

struct TextLayoutBackingTest : testing::Test {
  void SetUp() override {
    fakeheap::reset();
    Storage.reset();
  }
  void TearDown() override {
    EXPECT_TRUE(fakeheap::live.empty());
    Storage.reset();
  }
  std::vector<uint8_t> layout(int font, int scenario, bool* result = nullptr) {
    BlockStyle style;
    if (scenario == 1) {
      style.directionDefined = true;
      style.isRtl = true;
    }
    if (scenario == 3) {
      style.marginLeft = 12;
      style.marginRight = 12;
    }
    ParsedText text(false, false, false, scenario == 4, scenario == 5, 0, style);
    const std::vector<std::string> words =
        scenario == 1 ? std::vector<std::string>{"שלום", "עולם", "עברית", "123"}
        : scenario == 2
            ? std::vector<std::string>{"日", "本", "語", "読", "書"}
            : std::vector<std::string>{"A", "paragraph", "with", "different", "word", "lengths", "and", "punctuation."};
    for (int i = 0; i < 400; ++i)
      text.addWord(words[i % words.size()], i % 3 ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD, false, scenario == 2,
                   false, 0, i * 8);
    if (scenario == 2) text.setRubyGroupAt(0, 2, "にほん");
    FsFile output;
    EXPECT_TRUE(Storage.openFileForWrite("test", "layout", output));
    GfxRenderer renderer;
    bool ok = text.layoutAndExtractLines(renderer, font, 240,
                                         [&](std::shared_ptr<TextBlock> block, uint32_t offset, uint32_t) {
                                           EXPECT_TRUE(block->valid());
                                           EXPECT_TRUE(block->serialize(output));
                                           EXPECT_EQ(output.write(&offset, sizeof(offset)), sizeof(offset));
                                         });
    output.close();
    if (result)
      *result = ok;
    else
      EXPECT_TRUE(ok);
    return Storage.bytes("layout");
  }
};
TEST_F(TextLayoutBackingTest, SameSerializedLinesAcrossPoolsFontsAndReadingAids) {
  std::ofstream reference;
  if (const char* path = std::getenv("LAYOUT_REFERENCE_OUT")) reference.open(path, std::ios::binary);
  for (int font : {0, 2, 5})
    for (int scenario = 0; scenario < 6; ++scenario) {
      fakeheap::reset(false);
      const auto expected = layout(font, scenario);
      ASSERT_FALSE(expected.empty());
      EXPECT_TRUE(fakeheap::live.empty());
      if (reference) reference.write(reinterpret_cast<const char*>(expected.data()), expected.size());
      fakeheap::reset();
      EXPECT_EQ(layout(font, scenario), expected);
      EXPECT_TRUE(fakeheap::live.empty());
      fakeheap::reset();
      fakeheap::external.fail = 1000;
      EXPECT_EQ(layout(font, scenario), expected);
      EXPECT_TRUE(fakeheap::live.empty());
    }
}
#ifndef PSRAM_REFERENCE
TEST_F(TextLayoutBackingTest, AllocationFailureReturnsWithoutPartialLinesOrLeaks) {
  fakeheap::external.fail = 1000;
  fakeheap::internal.largest = 1;
  bool ok = true;
  auto output = layout(0, 0, &ok);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(output.empty());
  EXPECT_TRUE(fakeheap::live.empty());
}
#endif

TEST_F(TextLayoutBackingTest, SizedLineSurvivesCacheRoundTrip) {
  BlockStyle style;
  style.fontSize = 32;
  style.lineHeight = 68;
  ParsedText text(false, false, false, false, false, 0, style);
  text.addWord("Heading", EpdFontFamily::BOLD, false, false, false, 0, 0);
  GfxRenderer renderer;
  FsFile output;
  ASSERT_TRUE(Storage.openFileForWrite("test", "sized-line", output));
  ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 480, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
    ASSERT_TRUE(block->serialize(output));
  }));
  output.close();
  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "sized-line", input));
  auto restored = TextBlock::deserialize(input);
  input.close();
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getBlockStyle().fontSize, 32);
  EXPECT_EQ(restored->getBlockStyle().lineHeight, 68);
  ASSERT_EQ(restored->wordCount(), 1);
  EXPECT_STREQ(restored->wordText(0), "Heading");
  EXPECT_EQ(restored->wordStyle(0), EpdFontFamily::BOLD);
}

namespace {

TextBlock cacheLineWithReadingAids() {
  BlockStyle style;
  style.alignment = CssTextAlign::Right;
  style.textAlignDefined = true;
  style.marginTop = -1;
  style.marginBottom = 2;
  style.marginLeft = -3;
  style.marginRight = 4;
  style.paddingTop = 5;
  style.paddingBottom = 6;
  style.paddingLeft = 7;
  style.paddingRight = 8;
  style.textIndent = -9;
  style.textIndentDefined = true;
  style.isRtl = true;
  style.directionDefined = true;
  style.fontSize = 32;
  style.lineHeight = 68;
  return TextBlock({"ffi", "読"}, {-7, 42}, {EpdFontFamily::BOLD, EpdFontFamily::ITALIC}, {1, 3}, {5, 9}, {2, 4},
                   {0x01, 0x06}, {false, false}, style, {"", "どく"});
}

const std::vector<uint8_t>& readingAidCacheBytes() {
  // Existing cache bytes: scalar header, packed word arena, ruby, scalar style.
  // Literal bytes make struct padding, field reordering and width changes visible.
  static const std::vector<uint8_t> bytes = {
      0x02, 0x00, 0x01, 0x01, 0x01, 0x01, 0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0xf9, 0xff, 0x2a, 0x00,
      0x05, 0x00, 0x09, 0x00, 0x02, 0x00, 0x04, 0x00, 0x01, 0x02, 0x01, 0x03, 0x01, 0x06, 0x00, 'f',
      'f',  'i',  0x00, 0xe8, 0xaa, 0xad, 0x00, 0x01, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0xe3,
      0x81, 0xa9, 0xe3, 0x81, 0x8f, 0x03, 0x01, 0xff, 0xff, 0x02, 0x00, 0xfd, 0xff, 0x04, 0x00, 0x05,
      0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0xf7, 0xff, 0x01, 0x01, 0x01, 0x20, 0x44, 0x00};
  return bytes;
}

}  // namespace

TEST_F(TextLayoutBackingTest, ReadingAidLineKeepsCacheBytesAndConsumerMetadata) {
  const auto line = cacheLineWithReadingAids();
  ASSERT_TRUE(line.valid());
  FsFile output;
  ASSERT_TRUE(Storage.openFileForWrite("test", "reading-aids", output));
  ASSERT_TRUE(line.serialize(output));
  EXPECT_EQ(output.position(), readingAidCacheBytes().size());
  output.close();
  EXPECT_EQ(Storage.bytes("reading-aids"), readingAidCacheBytes());

  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "reading-aids", input));
  const auto restored = TextBlock::deserialize(input);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(input.available(), 0);
  ASSERT_EQ(restored->wordCount(), 2);
  EXPECT_STREQ(restored->wordText(0), "ffi");
  EXPECT_STREQ(restored->wordText(1), "読");
  EXPECT_EQ(restored->wordXpos(0), -7);
  EXPECT_EQ(restored->wordXpos(1), 42);
  EXPECT_EQ(restored->wordStyle(0), EpdFontFamily::BOLD);
  EXPECT_EQ(restored->wordStyle(1), EpdFontFamily::ITALIC);
  EXPECT_EQ(restored->focusBoundary(0), 1);
  EXPECT_EQ(restored->focusBoundary(1), 3);
  EXPECT_EQ(restored->focusRunOffset(0), 5);
  EXPECT_EQ(restored->focusRunOffset(1), 9);
  EXPECT_EQ(restored->guideDotXOffset(0), 2);
  EXPECT_EQ(restored->guideDotXOffset(1), 4);
  EXPECT_EQ(restored->wordFlags(0), TextBlock::WORD_FLAG_BACKGROUND_BLACK);
  EXPECT_TRUE(restored->wordEndsWithInsertedHyphen(1));
  EXPECT_EQ(restored->wordLinkId(1), 1);
  EXPECT_FALSE(restored->wordHasSpaceBefore(0));
  EXPECT_FALSE(restored->wordHasSpaceBefore(1));
  ASSERT_EQ(restored->getRubyTexts().size(), 2U);
  EXPECT_TRUE(restored->getRubyTexts()[0].empty());
  EXPECT_EQ(restored->getRubyTexts()[1], "どく");
  const auto& style = restored->getBlockStyle();
  EXPECT_EQ(style.alignment, CssTextAlign::Right);
  EXPECT_TRUE(style.textAlignDefined);
  EXPECT_EQ(style.marginTop, -1);
  EXPECT_EQ(style.marginBottom, 2);
  EXPECT_EQ(style.marginLeft, -3);
  EXPECT_EQ(style.marginRight, 4);
  EXPECT_EQ(style.paddingTop, 5);
  EXPECT_EQ(style.paddingBottom, 6);
  EXPECT_EQ(style.paddingLeft, 7);
  EXPECT_EQ(style.paddingRight, 8);
  EXPECT_EQ(style.textIndent, -9);
  EXPECT_TRUE(style.textIndentDefined);
  EXPECT_TRUE(style.isRtl);
  EXPECT_TRUE(style.directionDefined);
  EXPECT_EQ(style.fontSize, 32);
  EXPECT_EQ(style.lineHeight, 68);
}

TEST_F(TextLayoutBackingTest, ReadingAidLineRejectsEveryShortWriteAndTruncatedPayload) {
  const auto line = cacheLineWithReadingAids();
  ASSERT_TRUE(line.valid());
  const auto& expected = readingAidCacheBytes();
  for (size_t cut = 0; cut < expected.size(); ++cut) {
    SCOPED_TRACE(cut);
    FsFile output;
    ASSERT_TRUE(Storage.openFileForWrite("test", "short-line", output));
    Storage.failWritesAt("short-line", cut);
    EXPECT_FALSE(line.serialize(output));
    EXPECT_EQ(output.position(), cut);
    output.close();
    EXPECT_EQ(Storage.bytes("short-line"), std::vector<uint8_t>(expected.begin(), expected.begin() + cut));
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "short-line", input));
    EXPECT_EQ(TextBlock::deserialize(input), nullptr);
  }
}

TEST_F(TextLayoutBackingTest, ReadingAidLineRejectsCorruptMetadataAndWordBoundaries) {
  const std::vector<std::pair<size_t, uint8_t>> corruptions = {
      {2, 2},    // Boolean header flag is not 0 or 1.
      {8, 1},    // First word does not start at the beginning of the text region.
      {10, 8},   // Second word offset lies beyond the text region.
      {38, 1},   // Last word is not NUL-terminated.
      {39, 3},   // Ruby count exceeds the word count.
      {41, 2},   // Ruby annotation refers to a missing word.
      {76, 255}  // Resolved font size is outside the supported range.
  };
  for (const auto& [offset, value] : corruptions) {
    SCOPED_TRACE(offset);
    auto bytes = readingAidCacheBytes();
    bytes[offset] = value;
    Storage.put("corrupt-line", std::move(bytes));
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "corrupt-line", input));
    EXPECT_EQ(TextBlock::deserialize(input), nullptr);
  }
}

namespace {

Page persistedTextPage() {
  auto line = std::shared_ptr<TextBlock>(new TextBlock(cacheLineWithReadingAids()));
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(line, -11, 13));
  page.elements.push_back(std::make_unique<PageHorizontalRule>(300, 2, -5, 7));
  TableFragmentCell cell;
  cell.isHeader = true;
  cell.colSpan = 2;
  cell.lines.push_back(line);
  TableFragmentRow row;
  row.height = 74;
  row.headerSeparator = true;
  row.cells.push_back(std::move(cell));
  page.elements.push_back(
      std::make_unique<PageTableFragment>(200, 2, 3, 68, std::vector<TableFragmentRow>{std::move(row)}, 10, 20));
  page.addFootnote("1", "#fn", 1);
  page.addPublisherPageMarker("iv", 50);
  return page;
}

std::vector<uint8_t> persistedTextPageBytes() {
  std::vector<uint8_t> bytes = {0x03, 0x00, 0x01, 0xf5, 0xff, 0x0d, 0x00};
  const auto& line = readingAidCacheBytes();
  bytes.insert(bytes.end(), line.begin(), line.end());
  bytes.insert(bytes.end(), {0x04, 0xfb, 0xff, 0x07, 0x00, 0x2c, 0x01, 0x02});
  bytes.insert(bytes.end(), {0x03, 0x0a, 0x00, 0x14, 0x00, 0xc8, 0x00, 0x02, 0x03, 0x44, 0x00, 0x01, 0x4a, 0x00, 0x01,
                             0x01, 0x01, 0x02, 0x01});
  bytes.insert(bytes.end(), line.begin(), line.end());
  bytes.insert(bytes.end(), {0x01, 0x00, '1'});
  bytes.insert(bytes.end(), 31, 0x00);
  bytes.insert(bytes.end(), {'#', 'f', 'n'});
  bytes.insert(bytes.end(), 93, 0x00);
  bytes.insert(bytes.end(), {0x01, 0x01, 0x32, 0x00, 'i', 'v'});
  bytes.insert(bytes.end(), 14, 0x00);
  return bytes;
}

}  // namespace

TEST_F(TextLayoutBackingTest, RealTextPageKeepsWireRecordsAndVisibleLineGeometry) {
  const auto page = persistedTextPage();
  FsFile output;
  ASSERT_TRUE(Storage.openFileForWrite("test", "page", output));
  ASSERT_TRUE(page.serialize(output));
  output.close();
  EXPECT_EQ(Storage.bytes("page"), persistedTextPageBytes());
  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "page", input));
  const auto restored = Page::deserialize(input);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(input.available(), 0);
  ASSERT_EQ(restored->elements.size(), 3U);
  EXPECT_EQ(restored->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(restored->elements[1]->getTag(), TAG_PageHorizontalRule);
  EXPECT_EQ(restored->elements[2]->getTag(), TAG_PageTableFragment);
  EXPECT_EQ(static_cast<const PageTableFragment&>(*restored->elements[2]).getHeight(), 75);
  std::vector<PageTextLine> lines;
  ASSERT_TRUE(restored->forEachTextLine(
      [](const PageTextLine& line, void* context) {
        static_cast<std::vector<PageTextLine>*>(context)->push_back(line);
        return true;
      },
      &lines));
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_EQ(lines[0].xPos, -11);
  EXPECT_EQ(lines[0].yPos, 13);
  EXPECT_FALSE(lines[0].isTableText);
  EXPECT_EQ(lines[1].xPos, 13);
  EXPECT_EQ(lines[1].yPos, 23);
  EXPECT_TRUE(lines[1].isTableText);
  EXPECT_EQ(lines[1].clipWidth, 194);
  EXPECT_EQ(lines[1].clipHeight, 68);
  EXPECT_EQ(lines[1].lineHeight, 68);
  for (const auto& line : lines) {
    ASSERT_NE(line.block, nullptr);
    ASSERT_EQ(line.block->wordCount(), 2);
    EXPECT_STREQ(line.block->wordText(0), "ffi");
    EXPECT_STREQ(line.block->wordText(1), "読");
    EXPECT_EQ(line.block->wordLinkId(1), 1);
    EXPECT_FALSE(line.block->wordHasSpaceBefore(1));
    EXPECT_EQ(line.block->getRubyTexts()[1], "どく");
  }
  ASSERT_EQ(restored->footnotes.size(), 1U);
  EXPECT_STREQ(restored->footnotes[0].number, "1");
  EXPECT_STREQ(restored->footnotes[0].href, "#fn");
  EXPECT_EQ(restored->footnotes[0].linkId, 1);
  ASSERT_EQ(restored->publisherPageMarkers.size(), 1U);
  EXPECT_EQ(restored->publisherPageMarkers[0].yPos, 50);
  EXPECT_STREQ(restored->publisherPageMarkers[0].label, "iv");
}

TEST_F(TextLayoutBackingTest, RealTextPageRejectsEveryShortWriteAndTruncatedRecord) {
  const auto page = persistedTextPage();
  const auto expected = persistedTextPageBytes();
  for (size_t cut = 0; cut < expected.size(); ++cut) {
    SCOPED_TRACE(cut);
    FsFile output;
    ASSERT_TRUE(Storage.openFileForWrite("test", "short-page", output));
    Storage.failWritesAt("short-page", cut);
    EXPECT_FALSE(page.serialize(output));
    EXPECT_EQ(output.position(), cut);
    output.close();
    EXPECT_EQ(Storage.bytes("short-page"), std::vector<uint8_t>(expected.begin(), expected.begin() + cut));
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "short-page", input));
    EXPECT_EQ(Page::deserialize(input), nullptr);
  }
}
