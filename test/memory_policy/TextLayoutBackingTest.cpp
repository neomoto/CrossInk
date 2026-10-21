#include <Arduino.h>
#include <Epub/Page.h>
#include <Epub/ParsedText.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Serialization.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>

#ifndef PSRAM_REFERENCE
#include <Epub/IdlePagePreparation.h>
#include <Epub/PageReadBudget.h>
#endif
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

TEST_F(TextLayoutBackingTest, MixedWidthPodGroupKeepsOutputsOnPartialReadsAndStopsAtRecordBoundary) {
  std::vector<uint8_t> bytes = {0x91, 0xfb, 0xff, 0x78, 0x56, 0x34, 0x12, 0x08,
                                0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x01};
  bytes.insert(bytes.end(), 48, 0xa5);
  bytes.back() = 0x2e;
  bytes.push_back(0xd7);  // The next record must not be consumed by the 64-byte group.
  for (size_t cut = 0; cut <= 64; ++cut) {
    SCOPED_TRACE(cut);
    Storage.put("mixed-pod-group", bytes);
    if (cut < 64) Storage.data("mixed-pod-group").readFailAt = cut;
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "mixed-pod-group", input));
    uint8_t first = 0x3c;
    int16_t signedValue = 777;
    uint32_t value = 0xaabbccdd;
    uint64_t wideValue = UINT64_C(0xfedcba9876543210);
    bool flag = false;
    uint8_t payload[48];
    std::fill_n(payload, sizeof(payload), 0x6a);
    const bool complete = serialization::tryReadPodGroup(input, first, signedValue, value, wideValue, flag, payload);
    ASSERT_EQ(complete, cut == 64);
    if (!complete) {
      EXPECT_EQ(first, 0x3c);
      EXPECT_EQ(signedValue, 777);
      EXPECT_EQ(value, 0xaabbccddU);
      EXPECT_EQ(wideValue, UINT64_C(0xfedcba9876543210));
      EXPECT_FALSE(flag);
      EXPECT_EQ(std::vector<uint8_t>(payload, payload + sizeof(payload)), std::vector<uint8_t>(48, 0x6a));
      input.close();
      continue;
    }
    EXPECT_EQ(first, 0x91);
    EXPECT_EQ(signedValue, -5);
    EXPECT_EQ(value, 0x12345678U);
    EXPECT_EQ(wideValue, UINT64_C(0x0102030405060708));
    EXPECT_TRUE(flag);
    EXPECT_EQ(std::vector<uint8_t>(payload, payload + sizeof(payload)),
              std::vector<uint8_t>(bytes.begin() + 16, bytes.begin() + 64));
    EXPECT_EQ(input.available(), 1);
    uint8_t nextRecord = 0;
    ASSERT_TRUE(serialization::tryReadPod(input, nextRecord));
    EXPECT_EQ(nextRecord, 0xd7);
    input.close();
  }
}

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

TEST_F(TextLayoutBackingTest, ReadingAidLineRejectsPartialReadsBeforePhysicalEndOfFile) {
  const auto& bytes = readingAidCacheBytes();
  for (size_t cut = 0; cut < bytes.size(); ++cut) {
    SCOPED_TRACE(cut);
    Storage.put("partial-line", bytes);
    Storage.data("partial-line").readFailAt = cut;
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "partial-line", input));
    EXPECT_EQ(TextBlock::deserialize(input), nullptr);
    input.close();
  }
}

TEST_F(TextLayoutBackingTest, PartialTableHeadersKeepExistingOutputMetadataAndLines) {
  auto line = std::shared_ptr<TextBlock>(new TextBlock(cacheLineWithReadingAids()));
  for (size_t cut = 0; cut < 3; ++cut) {
    SCOPED_TRACE(cut);
    Storage.put("partial-cell-header", {0x01, 0x02, 0x00});
    Storage.data("partial-cell-header").readFailAt = cut;
    TableFragmentCell cell;
    cell.colSpan = 7;
    cell.lines.push_back(line);
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "partial-cell-header", input));
    EXPECT_FALSE(TableFragmentCell::deserialize(input, cell));
    EXPECT_FALSE(cell.isHeader);
    EXPECT_EQ(cell.colSpan, 7);
    ASSERT_EQ(cell.lines.size(), 1U);
    EXPECT_EQ(cell.lines[0], line);
    input.close();
  }
  for (size_t cut = 0; cut < 4; ++cut) {
    SCOPED_TRACE(cut);
    Storage.put("partial-row-header", {0x4a, 0x00, 0x01, 0x00});
    Storage.data("partial-row-header").readFailAt = cut;
    TableFragmentRow row;
    row.height = 99;
    TableFragmentCell cell;
    cell.colSpan = 3;
    cell.lines.push_back(line);
    row.cells.push_back(std::move(cell));
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "partial-row-header", input));
    EXPECT_FALSE(TableFragmentRow::deserialize(input, row));
    EXPECT_EQ(row.height, 99);
    EXPECT_FALSE(row.headerSeparator);
    ASSERT_EQ(row.cells.size(), 1U);
    EXPECT_EQ(row.cells[0].colSpan, 3);
    ASSERT_EQ(row.cells[0].lines.size(), 1U);
    EXPECT_EQ(row.cells[0].lines[0], line);
    input.close();
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

TEST_F(TextLayoutBackingTest, RealTextPageRejectsPartialReadsBeforePhysicalEndOfFile) {
  const auto bytes = persistedTextPageBytes();
  for (size_t cut = 0; cut < bytes.size(); ++cut) {
    SCOPED_TRACE(cut);
    Storage.put("partial-page", bytes);
    Storage.data("partial-page").readFailAt = cut;
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "partial-page", input));
    EXPECT_EQ(Page::deserialize(input), nullptr);
    input.close();
  }
}

#ifndef PSRAM_REFERENCE
namespace {

std::vector<uint8_t> idlePageBytes(const Page& page) {
  FsFile output;
  EXPECT_TRUE(Storage.openFileForWrite("test", "idle-serialized", output));
  EXPECT_TRUE(page.serialize(output));
  output.close();
  return Storage.bytes("idle-serialized");
}

Page singleLineIdlePage(const std::string& text) {
  auto line = std::make_shared<TextBlock>(std::vector<std::string>{text}, std::vector<int16_t>{17},
                                          std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR},
                                          std::vector<uint8_t>{}, std::vector<uint16_t>{}, std::vector<uint16_t>{},
                                          std::vector<uint8_t>{}, std::vector<bool>{false});
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(std::move(line), 3, 19));
  return page;
}

std::unique_ptr<Page> loadIdleFixture() {
  const auto bytes = persistedTextPageBytes();
  Storage.put("idle-fixture", bytes);
  FsFile input;
  if (!Storage.openFileForRead("test", "idle-fixture", input)) return nullptr;
  return Page::deserializeForIdle(input, bytes.size());
}

IdlePageKey idleKey() {
  static const int book = 1;
  static const int section = 2;
  IdlePageKey key;
  key.book = &book;
  key.section = &section;
  key.sectionGeneration = 7;
  key.spine = 3;
  key.page = 5;
  key.fontGeneration = 11;
  key.layout.fontId = 12;
  key.layout.lineCompression = 1.0003f;
  key.layout.viewportWidth = 480;
  key.layout.viewportHeight = 700;
  return key;
}

}  // namespace

TEST_F(TextLayoutBackingTest, IdlePageKeepsRealTextTableRubyFootnoteAndPublisherAnchors) {
  auto page = loadIdleFixture();
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(idlePageBytes(*page), persistedTextPageBytes());
  const auto& line = static_cast<const PageLine&>(*page->elements[0]);
  EXPECT_EQ(line.xPos, -11);
  EXPECT_EQ(line.yPos, 13);
  EXPECT_STREQ(line.getBlock()->wordText(1), "読");
  EXPECT_EQ(line.getBlock()->getRubyTexts()[1], "どく");
  EXPECT_EQ(page->footnotes[0].linkId, 1);
  EXPECT_STREQ(page->footnotes[0].href, "#fn");
  EXPECT_EQ(page->publisherPageMarkers[0].yPos, 50);
}

TEST_F(TextLayoutBackingTest, IdlePageNeverBorrowsTheFollowingPagesBytesToCompleteATruncatedRecord) {
  const auto complete = persistedTextPageBytes();
  for (size_t cut = 0; cut < complete.size(); ++cut) {
    SCOPED_TRACE(cut);
    std::vector<uint8_t> bytes(complete.begin(), complete.begin() + cut);
    bytes.insert(bytes.end(), complete.begin(), complete.end());
    Storage.put("adjacent-pages", bytes);
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "adjacent-pages", input));
    EXPECT_EQ(Page::deserializeForIdle(input, cut), nullptr);
    EXPECT_LE(input.position(), cut);
    ASSERT_TRUE(input.seek(cut));
    auto requested = Page::deserializeForIdle(input, bytes.size());
    ASSERT_NE(requested, nullptr);
    EXPECT_STREQ(requested->footnotes[0].href, "#fn");
    EXPECT_EQ(Storage.bytes("adjacent-pages"), bytes);
  }
}

TEST_F(TextLayoutBackingTest, IdlePageRejectsTrailingSpanGarbageWhileTheRequestedPageKeepsItsOriginalRecords) {
  auto bytes = persistedTextPageBytes();
  const auto original = bytes;
  bytes.push_back(0xa5);
  Storage.put("idle-trailing", bytes);
  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "idle-trailing", input));
  EXPECT_EQ(Page::deserializeForIdle(input, bytes.size()), nullptr);
  ASSERT_TRUE(input.seek(0));
  auto requested = Page::deserialize(input);
  ASSERT_NE(requested, nullptr);
  EXPECT_EQ(idlePageBytes(*requested), original);
}

TEST_F(TextLayoutBackingTest, IdlePageShortReadsLeaveTheNormalRequestedPageAndAnchorsAvailable) {
  const auto bytes = persistedTextPageBytes();
  for (size_t cut = 0; cut < bytes.size(); ++cut) {
    SCOPED_TRACE(cut);
    Storage.put("idle-short-read", bytes);
    Storage.data("idle-short-read").readFailAt = cut;
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "idle-short-read", input));
    EXPECT_EQ(Page::deserializeForIdle(input, bytes.size()), nullptr);
    Storage.data("idle-short-read").readFailAt = std::numeric_limits<size_t>::max();
    ASSERT_TRUE(input.seek(0));
    auto requested = Page::deserialize(input);
    ASSERT_NE(requested, nullptr);
    EXPECT_EQ(idlePageBytes(*requested), bytes);
  }
}

TEST_F(TextLayoutBackingTest, IdlePageRejectsMalformedNestedCountsBooleanFlagsAndRubyLengths) {
  const size_t line = 7;
  const size_t table = line + readingAidCacheBytes().size() + 8;
  const size_t nestedLine = table + 19;
  const size_t footnotes = nestedLine + readingAidCacheBytes().size();
  const size_t markers = footnotes + 2 + sizeof(FootnoteEntry);
  const std::vector<std::pair<size_t, uint8_t>> corruptions = {{0, 0xff},
                                                               {1, 0xff},
                                                               {2, 0xff},
                                                               {line, 0xff},
                                                               {line + 1, 0xff},
                                                               {line + 2, 2},
                                                               {line + 6, 0xff},
                                                               {line + 7, 0xff},
                                                               {line + 39, 3},
                                                               {line + 41, 2},
                                                               {line + 46, 0xff},
                                                               {line + 54, 2},
                                                               {table + 7, 9},
                                                               {table + 11, 65},
                                                               {table + 14, 2},
                                                               {table + 15, 9},
                                                               {table + 16, 2},
                                                               {table + 17, 0},
                                                               {table + 18, 65},
                                                               {nestedLine + 39, 3},
                                                               {nestedLine + 46, 0xff},
                                                               {footnotes, 17},
                                                               {markers, 9}};
  for (const auto& [offset, value] : corruptions) {
    SCOPED_TRACE(offset);
    auto bytes = persistedTextPageBytes();
    bytes.at(offset) = value;
    Storage.put("idle-corrupt", bytes);
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "idle-corrupt", input));
    EXPECT_EQ(Page::deserializeForIdle(input, bytes.size()), nullptr);
    EXPECT_EQ(Storage.bytes("idle-corrupt"), bytes);
  }
}

TEST_F(TextLayoutBackingTest, IdlePageAcceptsTheExactSerializedByteLimitButDeclinesOneByteMore) {
  const size_t fixedBytes = idlePageBytes(singleLineIdlePage("x")).size() - 1;
  const std::string text(PageReadBudget::MAX_SERIALIZED_BYTES - fixedBytes, 'a');
  const auto bytes = idlePageBytes(singleLineIdlePage(text));
  ASSERT_EQ(bytes.size(), PageReadBudget::MAX_SERIALIZED_BYTES);
  Storage.put("idle-byte-limit", bytes);
  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "idle-byte-limit", input));
  auto accepted = Page::deserializeForIdle(input, bytes.size());
  ASSERT_NE(accepted, nullptr);
  EXPECT_EQ(static_cast<const PageLine&>(*accepted->elements[0]).getBlock()->wordTextLen(0), text.size());
  input.close();
  const auto oversized = idlePageBytes(singleLineIdlePage(text + "b"));
  ASSERT_EQ(oversized.size(), PageReadBudget::MAX_SERIALIZED_BYTES + 1);
  Storage.put("idle-byte-limit", oversized);
  ASSERT_TRUE(Storage.openFileForRead("test", "idle-byte-limit", input));
  EXPECT_EQ(Page::deserializeForIdle(input, oversized.size()), nullptr);
  ASSERT_TRUE(input.seek(0));
  auto requested = Page::deserialize(input);
  ASSERT_NE(requested, nullptr);
  EXPECT_EQ(static_cast<const PageLine&>(*requested->elements[0]).getBlock()->wordTextLen(0), text.size() + 1);
}

TEST_F(TextLayoutBackingTest, IdlePageNestedAllocationsHonorTheExactAccountedByteAdmissionBoundary) {
  const auto bytes = persistedTextPageBytes();
  Storage.put("idle-charge-limit", bytes);
  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "idle-charge-limit", input));
  PageReadBudget initial(0, bytes.size());
  auto page = Page::deserialize(input, &initial);
  ASSERT_NE(page, nullptr);
  const size_t required = initial.chargedBytes();
  ASSERT_GT(required, 0U);
  ASSERT_LE(required, PageReadBudget::MAX_ALLOCATION_CHARGE);
  page.reset();
  for (const size_t limit : {required - 1, required}) {
    SCOPED_TRACE(limit);
    ASSERT_TRUE(input.seek(0));
    PageReadBudget budget(0, bytes.size(), limit);
    auto restored = Page::deserialize(input, &budget);
    EXPECT_EQ(restored != nullptr, limit == required);
    EXPECT_LE(budget.chargedBytes(), limit);
  }
}

TEST_F(TextLayoutBackingTest, IdlePageRejectsNestedAllocationAmplificationEvenWithinTheSerializedByteLimit) {
  Page page;
  const auto source = singleLineIdlePage("a");
  const auto line = static_cast<const PageLine&>(*source.elements[0]).getBlock();
  for (int i = 0; i < 96; ++i) page.elements.push_back(std::make_unique<PageLine>(line, 0, i));
  const auto bytes = idlePageBytes(page);
  ASSERT_LT(bytes.size(), PageReadBudget::MAX_SERIALIZED_BYTES);
  Storage.put("idle-amplification", bytes);
  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "idle-amplification", input));
  EXPECT_EQ(Page::deserializeForIdle(input, bytes.size()), nullptr);
  ASSERT_TRUE(input.seek(0));
  auto requested = Page::deserialize(input);
  ASSERT_NE(requested, nullptr);
  EXPECT_EQ(requested->elements.size(), 96U);
  EXPECT_EQ(static_cast<const PageLine&>(*requested->elements.back()).yPos, 95);
}

TEST_F(TextLayoutBackingTest, PreparedSuccessorIsConsumedOnceWithIdenticalConsumerRecords) {
  IdlePagePreparation prepared;
  const auto key = idleKey();
  ASSERT_TRUE(prepared.retain(loadIdleFixture(), key, 64U * 1024U, 40U * 1024U));
  auto page = prepared.take(key, 64U * 1024U, 40U * 1024U);
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(idlePageBytes(*page), persistedTextPageBytes());
  EXPECT_EQ(prepared.take(key, 64U * 1024U, 40U * 1024U), nullptr);
}

TEST_F(TextLayoutBackingTest, PreparedSuccessorRejectsEveryIdentityChangeIncludingAFullLayoutSignatureCollision) {
  const auto key = idleKey();
  std::vector<IdlePageKey> changed(21, key);
  changed[0].book = nullptr;
  changed[1].section = nullptr;
  ++changed[2].sectionGeneration;
  ++changed[3].spine;
  --changed[4].page;  // Current-page redraw/backward turn, not the successor.
  ++changed[5].page;  // Unrelated jump past the successor.
  ++changed[6].fontGeneration;
  ++changed[7].layout.fontId;
  changed[8].layout.lineCompression = 1.0004f;
  changed[9].layout.extraParagraphSpacing = true;
  changed[10].layout.forceParagraphIndents = true;
  ++changed[11].layout.paragraphAlignment;
  ++changed[12].layout.viewportWidth;
  ++changed[13].layout.viewportHeight;
  changed[14].layout.hyphenationEnabled = true;
  changed[15].layout.embeddedStyle = false;
  ++changed[16].layout.imageRendering;
  changed[17].layout.focusReadingEnabled = true;
  changed[18].layout.guideReadingEnabled = true;
  ++changed[19].layout.wordSpacing;
  changed[20].layout.renderMode = EpubRenderMode::Light;
  ASSERT_EQ(readerRenderSpecSignature(key.layout), readerRenderSpecSignature(changed[8].layout));
  for (const auto& requested : changed) {
    IdlePagePreparation prepared;
    ASSERT_TRUE(prepared.retain(loadIdleFixture(), key, 80U * 1024U, 56U * 1024U));
    EXPECT_EQ(prepared.take(requested, 80U * 1024U, 56U * 1024U), nullptr);
    EXPECT_EQ(prepared.take(key, 80U * 1024U, 56U * 1024U), nullptr);
  }
}

TEST_F(TextLayoutBackingTest, PreparedNonHitReleasesAllSharedPageBackingBeforeAForegroundFallback) {
  const auto key = idleKey();
  auto page = loadIdleFixture();
  ASSERT_NE(page, nullptr);
  std::weak_ptr<TextBlock> backing = static_cast<const PageLine&>(*page->elements[0]).getBlock();
  IdlePagePreparation prepared;
  ASSERT_TRUE(prepared.retain(std::move(page), key, 80U * 1024U, 56U * 1024U));
  auto redraw = key;
  --redraw.page;
  EXPECT_EQ(prepared.take(redraw, 80U * 1024U, 56U * 1024U), nullptr);
  EXPECT_TRUE(backing.expired());
  auto fallback = loadIdleFixture();
  ASSERT_NE(fallback, nullptr);
  EXPECT_EQ(idlePageBytes(*fallback), persistedTextPageBytes());
}

TEST_F(TextLayoutBackingTest, PreparedSuccessorRequiresBothFreeAndContiguousAdmissionAndRetentionFloors) {
  EXPECT_TRUE(IdlePagePreparation::canPrepare(80U * 1024U, 56U * 1024U));
  EXPECT_FALSE(IdlePagePreparation::canPrepare(80U * 1024U - 1, 128U * 1024U));
  EXPECT_FALSE(IdlePagePreparation::canPrepare(128U * 1024U, 56U * 1024U - 1));
  const std::pair<uint32_t, uint32_t> pressured[] = {{64U * 1024U - 1, 80U * 1024U}, {80U * 1024U, 40U * 1024U - 1}};
  for (const auto& [free, contiguous] : pressured) {
    const auto key = idleKey();
    IdlePagePreparation prepared;
    EXPECT_FALSE(prepared.retain(loadIdleFixture(), key, free, contiguous));
    EXPECT_EQ(prepared.take(key, 80U * 1024U, 56U * 1024U), nullptr);
    auto page = loadIdleFixture();
    ASSERT_NE(page, nullptr);
    std::weak_ptr<TextBlock> backing = static_cast<const PageLine&>(*page->elements[0]).getBlock();
    ASSERT_TRUE(prepared.retain(std::move(page), key, 80U * 1024U, 56U * 1024U));
    prepared.discardUnderPressure(free, contiguous);
    EXPECT_TRUE(backing.expired());
    EXPECT_EQ(prepared.take(key, 80U * 1024U, 56U * 1024U), nullptr);
    ASSERT_TRUE(prepared.retain(loadIdleFixture(), key, 80U * 1024U, 56U * 1024U));
    EXPECT_EQ(prepared.take(key, free, contiguous), nullptr);
    EXPECT_EQ(prepared.take(key, 80U * 1024U, 56U * 1024U), nullptr);
  }
}

TEST_F(TextLayoutBackingTest, PreparedNavigationKeepsTheForwardSuccessorButReleasesBackwardResidencyImmediately) {
  std::mutex readerMutex;
  for (const bool forward : {true, false}) {
    IdlePagePreparation prepared;
    const auto key = idleKey();
    auto page = loadIdleFixture();
    ASSERT_NE(page, nullptr);
    std::weak_ptr<TextBlock> backing = static_cast<const PageLine&>(*page->elements[0]).getBlock();
    ASSERT_TRUE(prepared.retain(std::move(page), key, 80U * 1024U, 56U * 1024U));
    prepared.beforePageTurn(forward, [&readerMutex] { return std::unique_lock<std::mutex>(readerMutex); });
    EXPECT_EQ(backing.expired(), !forward);
    auto consumed = prepared.take(key, 80U * 1024U, 56U * 1024U);
    EXPECT_EQ(consumed != nullptr, forward);
  }
}

TEST_F(TextLayoutBackingTest, PreparedLoaderReturnsExactPreparedRecordsOnceThenTheRealForegroundResult) {
  IdlePagePreparation prepared;
  const auto key = idleKey();
  ASSERT_TRUE(prepared.retain(loadIdleFixture(), key, 80U * 1024U, 56U * 1024U));
  const auto fallbackBytes = idlePageBytes(singleLineIdlePage("foreground"));
  Storage.put("foreground-page", fallbackBytes);
  const auto foreground = [] {
    FsFile input;
    if (!Storage.openFileForRead("test", "foreground-page", input)) return std::unique_ptr<Page>{};
    return Page::deserialize(input);
  };
  auto first = prepared.load(key, 80U * 1024U, 56U * 1024U, foreground);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(idlePageBytes(*first), persistedTextPageBytes());
  first.reset();
  auto second = prepared.load(key, 80U * 1024U, 56U * 1024U, foreground);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(idlePageBytes(*second), fallbackBytes);
}

TEST_F(TextLayoutBackingTest, PreparedLoaderReclaimsMismatchedAndPressuredBackingBeforeDeserializingForeground) {
  const auto key = idleKey();
  for (const int scenario : {0, 1, 2}) {
    IdlePagePreparation prepared;
    auto page = loadIdleFixture();
    ASSERT_NE(page, nullptr);
    std::weak_ptr<TextBlock> backing = static_cast<const PageLine&>(*page->elements[0]).getBlock();
    ASSERT_TRUE(prepared.retain(std::move(page), key, 80U * 1024U, 56U * 1024U));
    auto requested = key;
    if (scenario == 0) --requested.page;
    const uint32_t free = scenario == 1 ? 64U * 1024U - 1 : 80U * 1024U;
    const uint32_t contiguous = scenario == 2 ? 40U * 1024U - 1 : 56U * 1024U;
    auto foreground = prepared.load(requested, free, contiguous, [&backing] {
      EXPECT_TRUE(backing.expired());
      return loadIdleFixture();
    });
    ASSERT_NE(foreground, nullptr);
    EXPECT_EQ(idlePageBytes(*foreground), persistedTextPageBytes());
    EXPECT_EQ(prepared.take(key, 80U * 1024U, 56U * 1024U), nullptr);
  }
}
#endif
