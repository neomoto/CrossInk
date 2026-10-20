#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "TxtPageIndex.h"
#include "TxtPageLayout.h"

namespace {

using Lines = std::vector<std::string>;

struct Page {
  size_t consumed = 0;
  Lines lines;
};

int monospaceWidth(const char* text) {
  auto* cursor = reinterpret_cast<const unsigned char*>(text);
  int width = 0;
  while (*cursor) {
    utf8NextCodepoint(&cursor);
    ++width;
  }
  return width;
}

template <typename MeasureWidth>
Page readPage(const std::string& text, const size_t offset, const int maxLines, const int width,
              MeasureWidth&& measureWidth, const size_t chunkBytes = std::string::npos) {
  Page page;
  const size_t length = std::min(chunkBytes, text.size() - offset);
  page.consumed =
      txtlayout::parsePage(reinterpret_cast<const uint8_t*>(text.data() + offset), length, offset, text.size(),
                           maxLines, width, std::forward<MeasureWidth>(measureWidth), page.lines);
  return page;
}

Page readMonospacePage(const std::string& text, const size_t offset, const int maxLines, const int width,
                       const size_t chunkBytes = std::string::npos) {
  return readPage(text, offset, maxLines, width, monospaceWidth, chunkBytes);
}

Page readAlignedPage(const std::string& text, const size_t offset, const int maxLines, const int width,
                     const size_t preferredBytes) {
  const size_t available = std::min(preferredBytes + txtlayout::CHUNK_LOOKAHEAD_BYTES, text.size() - offset);
  const size_t length =
      txtlayout::alignedChunkLength(reinterpret_cast<const uint8_t*>(text.data() + offset), preferredBytes, available);
  return readMonospacePage(text, offset, maxLines, width, length);
}

std::string concatenate(const Lines& lines) {
  std::string text;
  for (const auto& line : lines) text += line;
  return text;
}

void expectValidUtf8(const Lines& lines) {
  for (const auto& line : lines) {
    auto* cursor = reinterpret_cast<const unsigned char*>(line.c_str());
    while (*cursor) {
      EXPECT_NE(utf8NextCodepoint(&cursor), static_cast<uint32_t>(REPLACEMENT_GLYPH)) << line;
    }
  }
}

struct IndexedText {
  txtlayout::PageIndexSpec spec;
  int height = 0;
  std::vector<size_t> offsets;
  std::vector<Lines> pages;
};

IndexedText indexText(const std::string& text, const int width, const int height, const int lines) {
  IndexedText index{{static_cast<uint32_t>(text.size()), width, lines, 3, 4, 6, 0}, height, {0}, {}};
  size_t offset = 0;
  while (offset < text.size()) {
    const auto page = readMonospacePage(text, offset, lines, width);
    EXPECT_GT(page.consumed, 0U);
    if (page.consumed == 0) break;
    index.pages.push_back(page.lines);
    offset += page.consumed;
    if (offset < text.size()) index.offsets.push_back(offset);
  }
  EXPECT_EQ(offset, text.size());
  return index;
}

std::string rotationText() {
  std::string text;
  for (int paragraph = 0; paragraph < 24; ++paragraph) {
    text += "alpha beta gamma delta epsilon zeta eta theta\r\n";
    text += "\r\nUTF-8 caf\xC3\xA9 and \xE4\xB8\xAD words stay at their exact source bytes.\n";
  }
  return text;
}

void expectIndexedPages(const std::string& text, const IndexedText& index, const std::vector<size_t>& offsets) {
  ASSERT_EQ(offsets, index.offsets);
  ASSERT_EQ(index.pages.size(), offsets.size());
  for (size_t page = 0; page < offsets.size(); ++page) {
    const auto actual = readMonospacePage(text, offsets[page], index.spec.linesPerPage, index.spec.viewportWidth);
    EXPECT_EQ(actual.lines, index.pages[page]) << page;
    const size_t next = page + 1 < offsets.size() ? offsets[page + 1] : text.size();
    EXPECT_EQ(offsets[page] + actual.consumed, next) << page;
  }
}

void putLegacyIndex(const IndexedText& index) {
  ASSERT_TRUE(txtlayout::savePageIndex("/cache/index.bin", index.spec, index.offsets, true));
  auto bytes = Storage.bytes("/cache/index.bin");
  bytes[4] = txtlayout::LEGACY_PAGE_INDEX_VERSION;
  Storage.put("/cache/index.bin", std::move(bytes));
}

void putLegacyProgress(const uint16_t page) {
  Storage.put("/cache/progress.bin", {static_cast<uint8_t>(page), static_cast<uint8_t>(page >> 8), 0, 0});
}

class TxtPageIndexTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

}  // namespace

TEST(TxtPageLayout, GreedyWordPagesContinueAtExactSourceBytes) {
  const std::string text = "alpha beta gamma delta epsilon zeta";
  const auto first = readMonospacePage(text, 0, 2, 10);
  EXPECT_EQ(first.lines, (Lines{"alpha beta", "gamma"}));
  ASSERT_EQ(first.consumed, 17u);

  const auto second = readMonospacePage(text, first.consumed, 2, 10);
  EXPECT_EQ(second.lines, (Lines{"delta", "epsilon"}));
  ASSERT_EQ(second.consumed, 14u);

  const auto third = readMonospacePage(text, first.consumed + second.consumed, 2, 10);
  EXPECT_EQ(third.lines, (Lines{"zeta"}));
  EXPECT_EQ(first.consumed + second.consumed + third.consumed, text.size());
}

TEST(TxtPageLayout, PagesCanContinueInsideAWordWithoutDroppingBytes) {
  const std::string text = "ABCDEFGHIJK";
  Lines lines;
  size_t offset = 0;
  for (const size_t expected : {4u, 4u, 3u}) {
    const auto page = readMonospacePage(text, offset, 1, 4);
    ASSERT_EQ(page.consumed, expected);
    lines.insert(lines.end(), page.lines.begin(), page.lines.end());
    offset += page.consumed;
  }
  EXPECT_EQ(lines, (Lines{"ABCD", "EFGH", "IJK"}));
  EXPECT_EQ(offset, text.size());
  EXPECT_EQ(concatenate(lines), text);
}

TEST(TxtPageLayout, AFollowingLongWordStartsOnTheNextVisualLine) {
  const std::string text = "aa abcdefghij";
  const auto first = readMonospacePage(text, 0, 2, 5);
  ASSERT_EQ(first.lines, (Lines{"aa", "abcde"}));
  ASSERT_EQ(first.consumed, 8u);
  const auto second = readMonospacePage(text, first.consumed, 2, 5);
  EXPECT_EQ(second.lines, (Lines{"fghij"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
  EXPECT_EQ(first.lines[0] + " " + first.lines[1] + concatenate(second.lines), text);
}

TEST(TxtPageLayout, FinalLineDoesNotConsumeANonexistentNewline) {
  const std::string text = "final line";
  const auto page = readMonospacePage(text, 0, 10, 30);
  EXPECT_EQ(page.lines, (Lines{text}));
  EXPECT_EQ(page.consumed, text.size());
}

TEST(TxtPageLayout, ActualNewlineIsConsumedEvenWhenThePageJustFilled) {
  const std::string text = "one\ntwo\n";
  const auto first = readMonospacePage(text, 0, 1, 30);
  EXPECT_EQ(first.lines, (Lines{"one"}));
  ASSERT_EQ(first.consumed, 4u);
  const auto second = readMonospacePage(text, first.consumed, 1, 30);
  EXPECT_EQ(second.lines, (Lines{"two"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
}

TEST(TxtPageLayout, CRLFAndBlankLinesKeepTheirSourceOffsets) {
  const std::string text = "\r\nalpha\r\n\r\nomega";
  const auto first = readMonospacePage(text, 0, 2, 30);
  EXPECT_EQ(first.lines, (Lines{"", "alpha"}));
  ASSERT_EQ(first.consumed, 9u);
  const auto second = readMonospacePage(text, first.consumed, 2, 30);
  EXPECT_EQ(second.lines, (Lines{"", "omega"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
}

TEST(TxtPageLayout, EmptyPhysicalLinesAdvanceOneNewlineAtATime) {
  const std::string text = "\n\nX\n";
  const auto first = readMonospacePage(text, 0, 1, 30);
  EXPECT_EQ(first.lines, (Lines{""}));
  ASSERT_EQ(first.consumed, 1u);
  const auto second = readMonospacePage(text, 1, 1, 30);
  EXPECT_EQ(second.lines, (Lines{""}));
  ASSERT_EQ(second.consumed, 1u);
  const auto third = readMonospacePage(text, 2, 1, 30);
  EXPECT_EQ(third.lines, (Lines{"X"}));
  EXPECT_EQ(third.consumed, 2u);
}

TEST(TxtPageLayout, TrailingCRAtEOFIsNotDisplayedOrOvershot) {
  const std::string text = "tail\r";
  const auto page = readMonospacePage(text, 0, 10, 30);
  EXPECT_EQ(page.lines, (Lines{"tail"}));
  EXPECT_EQ(page.consumed, text.size());
}

TEST(TxtPageLayout, EmptyFileHasNoPhantomLineOrConsumedByte) {
  const auto page = readMonospacePage("", 0, 10, 30);
  EXPECT_TRUE(page.lines.empty());
  EXPECT_EQ(page.consumed, 0u);
}

TEST(TxtPageLayout, LeadingAndRepeatedSpacesArePreservedApartFromOneWrapSpace) {
  const std::string text = "  ab   cd  ";
  const auto first = readMonospacePage(text, 0, 1, 5);
  EXPECT_EQ(first.lines, (Lines{"  ab "}));
  ASSERT_EQ(first.consumed, 6u);
  const auto second = readMonospacePage(text, first.consumed, 1, 5);
  EXPECT_EQ(second.lines, (Lines{" cd  "}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
  EXPECT_EQ(concatenate(first.lines) + " " + concatenate(second.lines), text);
}

TEST(TxtPageLayout, OnlyOneOfRepeatedWrapSeparatorsIsSkipped) {
  const std::string text = "ab  cd";
  const auto first = readMonospacePage(text, 0, 1, 2);
  EXPECT_EQ(first.lines, (Lines{"ab"}));
  ASSERT_EQ(first.consumed, 3u);
  const auto second = readMonospacePage(text, first.consumed, 2, 2);
  EXPECT_EQ(second.lines, (Lines{" c", "d"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
  EXPECT_EQ(concatenate(first.lines) + " " + concatenate(second.lines), text);
}

TEST(TxtPageLayout, SpaceOnlyParagraphStillMakesProgressAndKeepsRemainingSpace) {
  const std::string text = "   ";
  const auto first = readMonospacePage(text, 0, 1, 1);
  EXPECT_EQ(first.lines, (Lines{" "}));
  ASSERT_EQ(first.consumed, 2u);
  const auto second = readMonospacePage(text, first.consumed, 1, 1);
  EXPECT_EQ(second.lines, (Lines{" "}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
}

TEST(TxtPageLayout, UTF8LongWordsSplitOnlyAtWholeCodepoints) {
  const std::string text =
      "A\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80"
      "B\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80"
      "C";
  Lines lines;
  size_t offset = 0;
  for (const size_t expected : {6u, 7u, 8u}) {
    const auto page = readMonospacePage(text, offset, 1, 3);
    ASSERT_EQ(page.consumed, expected);
    lines.insert(lines.end(), page.lines.begin(), page.lines.end());
    offset += page.consumed;
  }
  EXPECT_EQ(offset, text.size());
  EXPECT_EQ(concatenate(lines), text);
  expectValidUtf8(lines);
}

TEST(TxtPageLayout, AnOversizedFirstCodepointStillAdvancesIntact) {
  const std::string text = "\xF0\x9F\x98\x80\xC3\xA9\xE4\xB8\xAD";
  const auto wide = [](const char* value) { return monospaceWidth(value) * 10; };
  const auto first = readPage(text, 0, 2, 3, wide);
  EXPECT_EQ(first.lines, (Lines{"\xF0\x9F\x98\x80", "\xC3\xA9"}));
  ASSERT_EQ(first.consumed, 6u);
  const auto second = readPage(text, first.consumed, 2, 3, wide);
  EXPECT_EQ(second.lines, (Lines{"\xE4\xB8\xAD"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
  expectValidUtf8(first.lines);
  expectValidUtf8(second.lines);
}

TEST(TxtPageLayout, ExactShapedPrefixCanFitWhenAdditiveGlyphWidthsWouldNot) {
  const std::string text = "ab cd ef";
  const auto contextual = [](const char* value) {
    if (std::string_view(value) == "ab cd") return 4;
    return monospaceWidth(value);
  };
  const auto page = readPage(text, 0, 10, 4, contextual);
  EXPECT_EQ(page.lines, (Lines{"ab cd", "ef"}));
  EXPECT_EQ(page.consumed, text.size());
}

TEST(TxtPageLayout, GreedyWrapStopsAtFirstOverflowEvenIfALaterWordWouldShrinkWidth) {
  const std::string text = "a b c";
  const auto nonmonotonic = [](const char* value) {
    if (std::string_view(value) == "a b") return 9;
    if (std::string_view(value) == "a b c") return 3;
    return monospaceWidth(value);
  };
  const auto page = readPage(text, 0, 10, 4, nonmonotonic);
  EXPECT_EQ(page.lines, (Lines{"a", "b c"}));
  EXPECT_EQ(page.consumed, text.size());
}

TEST(TxtPageLayout, AWholeFittingWordNeedNotHaveMonotonicCodepointPrefixes) {
  const std::string text = "abc de";
  const auto nonmonotonic = [](const char* value) {
    if (std::string_view(value) == "ab") return 9;
    if (std::string_view(value) == "abc") return 2;
    return monospaceWidth(value);
  };
  const auto page = readPage(text, 0, 10, 3, nonmonotonic);
  EXPECT_EQ(page.lines, (Lines{"abc", "de"}));
  EXPECT_EQ(page.consumed, text.size());
}

TEST(TxtPageLayout, AnOverflowingWordUsesForwardGreedyCodepointBoundariesToo) {
  const std::string text = "abcd";
  const auto nonmonotonic = [](const char* value) {
    if (std::string_view(value) == "ab") return 8;
    if (std::string_view(value) == "abc") return 2;
    if (std::string_view(value) == "abcd") return 9;
    return monospaceWidth(value);
  };
  const auto page = readPage(text, 0, 10, 4, nonmonotonic);
  EXPECT_EQ(page.lines, (Lines{"a", "bcd"}));
  EXPECT_EQ(page.consumed, text.size());
}

TEST(TxtPageLayout, AnUnfinishedFittingChunkAdvancesWithoutSkippingTheNextByte) {
  const std::string text = "abcdefgh";
  const auto first = readMonospacePage(text, 0, 10, 30, 4);
  EXPECT_EQ(first.lines, (Lines{"abcd"}));
  ASSERT_EQ(first.consumed, 4u);
  const auto second = readMonospacePage(text, first.consumed, 10, 30, 4);
  EXPECT_EQ(second.lines, (Lines{"efgh"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
  EXPECT_EQ(concatenate(first.lines) + concatenate(second.lines), text);
}

TEST(TxtPageLayout, APartialChunkDefersOnlyTheUnfinishedTailAfterOrdinaryWraps) {
  const std::string text = "abcdefghijk";
  Lines lines;
  size_t offset = 0;
  for (const size_t expected : {3u, 3u, 5u}) {
    const auto page = readMonospacePage(text, offset, 10, 3, 6);
    ASSERT_EQ(page.consumed, expected);
    lines.insert(lines.end(), page.lines.begin(), page.lines.end());
    offset += page.consumed;
  }
  EXPECT_EQ(lines, (Lines{"abc", "def", "ghi", "jk"}));
  EXPECT_EQ(offset, text.size());
  EXPECT_EQ(concatenate(lines), text);
}

TEST(TxtPageLayout, AnUnfinishedFollowingParagraphWaitsForTheNextPageRead) {
  const std::string text = "first\nsecondwordwithtail";
  const auto page = readMonospacePage(text, 0, 10, 5, 12);
  EXPECT_EQ(page.lines, (Lines{"first"}));
  EXPECT_EQ(page.consumed, 6u);
  EXPECT_EQ(text.substr(page.consumed), "secondwordwithtail");
}

TEST(TxtPageLayout, APartialUTF8TailIsReReadFromItsLeadByte) {
  const std::string text =
      "ab\xF0\x9F\x98\x80"
      "cd";
  const auto first = readMonospacePage(text, 0, 10, 30, 4);
  EXPECT_EQ(first.lines, (Lines{"ab"}));
  ASSERT_EQ(first.consumed, 2u);
  const auto second = readMonospacePage(text, first.consumed, 10, 30);
  EXPECT_EQ(second.lines, (Lines{"\xF0\x9F\x98\x80"
                                 "cd"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
  EXPECT_EQ(concatenate(first.lines) + concatenate(second.lines), text);
  expectValidUtf8(first.lines);
  expectValidUtf8(second.lines);
}

TEST(TxtPageLayout, AChunkContainingOnlyAnIncompleteCodepointIsNotEmitted) {
  const std::string text =
      "\xF0\x9F\x98\x80"
      "x";
  const auto page = readMonospacePage(text, 0, 10, 30, 2);
  EXPECT_TRUE(page.lines.empty());
  EXPECT_EQ(page.consumed, 0u);
}

TEST(TxtPageLayout, ZeroWidthUTF8AcrossAn8KBChunkCannotTruncateTheBook) {
  std::string text;
  for (int i = 0; i < 5000; ++i) text += "\xCC\x81";
  const auto zeroWidth = [](const char*) { return 0; };
  const auto first = readPage(text, 0, 10, 0, zeroWidth, 8192);
  ASSERT_EQ(first.consumed, 8192u);
  ASSERT_EQ(first.lines.size(), 1u);
  const auto second = readPage(text, first.consumed, 10, 0, zeroWidth, 8192);
  ASSERT_EQ(second.lines.size(), 1u);
  EXPECT_EQ(first.consumed + second.consumed, text.size());
  EXPECT_EQ(concatenate(first.lines) + concatenate(second.lines), text);
  expectValidUtf8(first.lines);
  expectValidUtf8(second.lines);
}

TEST(TxtPageLayout, BoundedLookaheadKeepsASplitCRLFOnTheSamePage) {
  const std::string text = "abcd\r\nEFGH";
  const auto first = readAlignedPage(text, 0, 10, 30, 5);
  EXPECT_EQ(first.lines, (Lines{"abcd"}));
  ASSERT_EQ(first.consumed, 6u);
  const auto second = readAlignedPage(text, first.consumed, 10, 30, 5);
  EXPECT_EQ(second.lines, (Lines{"EFGH"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
}

TEST(TxtPageLayout, BoundedLookaheadAlsoAbsorbsANewlineImmediatelyAfterTheCut) {
  const std::string text = "abcd\nEFGH";
  const auto first = readAlignedPage(text, 0, 10, 30, 4);
  EXPECT_EQ(first.lines, (Lines{"abcd"}));
  ASSERT_EQ(first.consumed, 5u);
  const auto second = readAlignedPage(text, first.consumed, 10, 30, 4);
  EXPECT_EQ(second.lines, (Lines{"EFGH"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
}

TEST(TxtPageLayout, BoundedLookaheadKeepsAllBytesOfASplitFourByteGlyph) {
  const std::string text =
      "abc\xF0\x9F\x98\x80"
      "def";
  const auto first = readAlignedPage(text, 0, 10, 30, 5);
  EXPECT_EQ(first.lines, (Lines{"abc\xF0\x9F\x98\x80"}));
  ASSERT_EQ(first.consumed, 7u);
  const auto second = readAlignedPage(text, first.consumed, 10, 30, 5);
  EXPECT_EQ(second.lines, (Lines{"def"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
  EXPECT_EQ(concatenate(first.lines) + concatenate(second.lines), text);
  expectValidUtf8(first.lines);
  expectValidUtf8(second.lines);
}

TEST(TxtPageLayout, SplitFourByteGlyphFollowedByCRLFUsesOnlyOneLineEnding) {
  const std::string text = "abc\xF0\x9F\x98\x80\r\nnext";
  const auto first = readAlignedPage(text, 0, 10, 30, 4);
  EXPECT_EQ(first.lines, (Lines{"abc\xF0\x9F\x98\x80"}));
  ASSERT_EQ(first.consumed, 9u);
  const auto second = readAlignedPage(text, first.consumed, 10, 30, 4);
  EXPECT_EQ(second.lines, (Lines{"next"}));
  EXPECT_EQ(first.consumed + second.consumed, text.size());
  expectValidUtf8(first.lines);
  expectValidUtf8(second.lines);
}

TEST(TxtPageLayout, PendingLivePositionWinsOverRetainedAndLegacyPositions) {
  EXPECT_EQ(txtlayout::preferredRestoreOffset(27000u, 26000u, 19008u), std::optional<size_t>{27000u});
  EXPECT_EQ(txtlayout::preferredRestoreOffset(0u, 26000u, 19008u), std::optional<size_t>{0u});
}

TEST(TxtPageLayout, RetainedLivePositionWinsWhenLegacyMigrationMustBeRepeated) {
  EXPECT_EQ(txtlayout::preferredRestoreOffset(std::nullopt, 27000u, 19008u), std::optional<size_t>{27000u});
}

TEST(TxtPageLayout, PersistedBookmarkIsUsedOnlyWithoutALivePosition) {
  EXPECT_EQ(txtlayout::preferredRestoreOffset(std::nullopt, std::nullopt, 19008u), std::optional<size_t>{19008u});
  EXPECT_EQ(txtlayout::preferredRestoreOffset(std::nullopt, std::nullopt, std::nullopt), std::nullopt);
}

TEST_F(TxtPageIndexTest, AlternatingGeometriesReloadExactPagesWithoutEvictingEitherLayout) {
  const auto text = rotationText();
  const auto narrow = indexText(text, 8, 30, 3);
  const auto wide = indexText(text, 20, 10, 1);
  ASSERT_NE(narrow.offsets, wide.offsets);
  const auto narrowPath = txtlayout::pageIndexPath("/cache", narrow.spec.viewportWidth, narrow.height);
  const auto widePath = txtlayout::pageIndexPath("/cache", wide.spec.viewportWidth, wide.height);
  ASSERT_TRUE(txtlayout::savePageIndex(narrowPath, narrow.spec, narrow.offsets, true));
  ASSERT_TRUE(txtlayout::savePageIndex(widePath, wide.spec, wide.offsets, true));
  const auto narrowBytes = Storage.bytes(narrowPath);
  const auto wideBytes = Storage.bytes(widePath);
  const size_t writes = Storage.writeOpens;

  std::vector<size_t> offsets;
  for (int rotation = 0; rotation < 6; ++rotation) {
    const auto& index = rotation % 2 ? wide : narrow;
    const auto path = txtlayout::pageIndexPath("/cache", index.spec.viewportWidth, index.height);
    ASSERT_TRUE(txtlayout::loadPageIndex(path, index.spec, offsets));
    expectIndexedPages(text, index, offsets);
    const size_t page = offsets.size() / 2;
    EXPECT_EQ(txtlayout::readPageIndexOffset(path, index.spec, page), offsets[page]);
  }
  EXPECT_EQ(Storage.writeOpens, writes);
  EXPECT_EQ(Storage.fileCount(), 2U);
  EXPECT_EQ(Storage.bytes(narrowPath), narrowBytes);
  EXPECT_EQ(Storage.bytes(widePath), wideBytes);
}

TEST_F(TxtPageIndexTest, EveryExistingSpecAndSourceFieldStillRejectsAStaleSlot) {
  const auto text = rotationText();
  const auto narrow = indexText(text, 8, 30, 3);
  const auto wide = indexText(text, 20, 10, 1);
  const auto narrowPath = txtlayout::pageIndexPath("/cache", 8, 30);
  const auto widePath = txtlayout::pageIndexPath("/cache", 20, 10);
  ASSERT_TRUE(txtlayout::savePageIndex(narrowPath, narrow.spec, narrow.offsets, true));
  ASSERT_TRUE(txtlayout::savePageIndex(widePath, wide.spec, wide.offsets, true));
  const auto narrowBytes = Storage.bytes(narrowPath);
  const auto wideBytes = Storage.bytes(widePath);
  for (int field = 0; field < 7; ++field) {
    auto changed = wide.spec;
    switch (field) {
      case 0:
        ++changed.sourceSize;
        break;
      case 1:
        ++changed.viewportWidth;
        break;
      case 2:
        ++changed.linesPerPage;
        break;
      case 3:
        ++changed.fontId;
        break;
      case 4:
        ++changed.verticalMargin;
        break;
      case 5:
        ++changed.horizontalMargin;
        break;
      case 6:
        ++changed.paragraphAlignment;
        break;
    }
    std::vector<size_t> offsets;
    EXPECT_FALSE(txtlayout::loadPageIndex(widePath, changed, offsets)) << field;
    EXPECT_FALSE(txtlayout::readPageIndexOffset(widePath, changed, 1).has_value()) << field;
    ASSERT_TRUE(txtlayout::loadPageIndex(narrowPath, narrow.spec, offsets));
    EXPECT_EQ(offsets, narrow.offsets);
    EXPECT_EQ(Storage.bytes(narrowPath), narrowBytes);
    EXPECT_EQ(Storage.bytes(widePath), wideBytes);
  }
}

TEST_F(TxtPageIndexTest, FailedWideWritesAndPromotionKeepBothCompletedLayoutsReadable) {
  const auto text = rotationText();
  const auto narrow = indexText(text, 8, 30, 3);
  const auto wide = indexText(text, 20, 10, 1);
  const auto replacement = indexText(text, 21, 10, 1);
  const auto narrowPath = txtlayout::pageIndexPath("/cache", 8, 30);
  const auto widePath = txtlayout::pageIndexPath("/cache", 20, 10);
  ASSERT_TRUE(txtlayout::savePageIndex(narrowPath, narrow.spec, narrow.offsets, true));
  ASSERT_TRUE(txtlayout::savePageIndex(widePath, wide.spec, wide.offsets, true));
  const auto narrowBytes = Storage.bytes(narrowPath);
  const auto wideBytes = Storage.bytes(widePath);

  Storage.failWrite = true;
  EXPECT_FALSE(txtlayout::savePageIndex(widePath, replacement.spec, replacement.offsets, true));
  EXPECT_FALSE(Storage.exists((widePath + ".tmp").c_str()));
  EXPECT_EQ(Storage.bytes(widePath), wideBytes);
  Storage.failRenameFrom = widePath + ".tmp";
  EXPECT_FALSE(txtlayout::savePageIndex(widePath, replacement.spec, replacement.offsets, true));
  EXPECT_EQ(Storage.bytes(widePath), wideBytes);
  EXPECT_EQ(Storage.bytes(narrowPath), narrowBytes);

  std::vector<size_t> offsets;
  ASSERT_TRUE(txtlayout::loadPageIndex(widePath, wide.spec, offsets));
  expectIndexedPages(text, wide, offsets);
  ASSERT_TRUE(Storage.rename(widePath.c_str(), (widePath + ".bak").c_str()));
  ASSERT_TRUE(txtlayout::loadPageIndex(widePath, wide.spec, offsets));
  expectIndexedPages(text, wide, offsets);
  ASSERT_TRUE(txtlayout::loadPageIndex(narrowPath, narrow.spec, offsets));
  expectIndexedPages(text, narrow, offsets);
  EXPECT_EQ(Storage.fileCount(), 2U);
}

TEST_F(TxtPageIndexTest, LegacyBookmarkUsesOriginalIndexEvenWithACompletedWideLayout) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  const auto wide = indexText(text, 20, 10, 1);
  putLegacyIndex(legacy);
  ASSERT_TRUE(txtlayout::savePageIndex("/cache/index.wide.bin", wide.spec, wide.offsets, true));
  constexpr uint16_t page = 5;
  ASSERT_NE(legacy.offsets[page], wide.offsets[page]);
  putLegacyProgress(page);
  const auto original = Storage.bytes("/cache/index.bin");

  // The same resolver is used before relayout and by the sleep thumbnail.
  EXPECT_EQ(txtlayout::readLegacyCacheOffset("/cache", text.size(), page), legacy.offsets[page]);
  const auto migration = txtlayout::migrateLegacyProgress("/cache", text.size());
  ASSERT_TRUE(migration.replacementSafe);
  ASSERT_EQ(migration.offset, legacy.offsets[page]);
  ASSERT_EQ(Storage.bytes("/cache/progress.bin").size(), 6U);
  EXPECT_EQ(txtlayout::readIndexUint32(Storage.bytes("/cache/progress.bin").data() + 2), legacy.offsets[page]);
  EXPECT_EQ(Storage.bytes("/cache/index.bin"), original);

  std::vector<size_t> offsets;
  ASSERT_TRUE(txtlayout::loadPageIndex("/cache/index.wide.bin", wide.spec, offsets));
  const auto next = std::upper_bound(offsets.begin(), offsets.end(), *migration.offset);
  const size_t widePage = std::max<size_t>(1, next - offsets.begin()) - 1;
  EXPECT_LE(offsets[widePage], *migration.offset);
  EXPECT_GT(offsets[widePage + 1], *migration.offset);
  expectIndexedPages(text, wide, offsets);
}

TEST_F(TxtPageIndexTest, FailedLegacyMigrationCannotOverwriteItsOnlySourceAndDoesNotReplaceLiveAnchors) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  const auto replacement = indexText(text, 9, 30, 3);
  putLegacyIndex(legacy);
  putLegacyProgress(5);
  const auto original = Storage.bytes("/cache/index.bin");
  Storage.failWritesAt("/cache/progress.bin", 2);

  for (int reopen = 0; reopen < 2; ++reopen) {
    const auto migration = txtlayout::migrateLegacyProgress("/cache", text.size());
    ASSERT_FALSE(migration.replacementSafe);
    ASSERT_EQ(migration.offset, legacy.offsets[5]);
    const auto writes = Storage.writeOpens;
    EXPECT_FALSE(
        txtlayout::savePageIndex("/cache/index.bin", replacement.spec, replacement.offsets, migration.replacementSafe));
    EXPECT_EQ(Storage.writeOpens, writes);
    EXPECT_EQ(Storage.bytes("/cache/index.bin"), original);
    EXPECT_EQ(txtlayout::readLegacyCacheOffset("/cache", text.size(), 5), legacy.offsets[5]);
    EXPECT_EQ(txtlayout::preferredRestoreOffset(700U, 600U, migration.offset), 700U);
    EXPECT_EQ(txtlayout::preferredRestoreOffset(std::nullopt, 600U, migration.offset), 600U);
  }
}

TEST_F(TxtPageIndexTest, SuccessfulLegacyMigrationAllowsNarrowCutoverWithoutChangingItsByteBookmark) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  const auto replacement = indexText(text, 9, 30, 3);
  putLegacyIndex(legacy);
  putLegacyProgress(5);
  const size_t anchor = legacy.offsets[5];
  const auto migration = txtlayout::migrateLegacyProgress("/cache", text.size());
  ASSERT_TRUE(migration.replacementSafe);
  ASSERT_TRUE(
      txtlayout::savePageIndex("/cache/index.bin", replacement.spec, replacement.offsets, migration.replacementSafe));
  EXPECT_EQ(txtlayout::readIndexUint32(Storage.bytes("/cache/progress.bin").data() + 2), anchor);
  EXPECT_FALSE(txtlayout::readLegacyCacheOffset("/cache", text.size(), 5).has_value());
  std::vector<size_t> offsets;
  ASSERT_TRUE(txtlayout::loadPageIndex("/cache/index.bin", replacement.spec, offsets));
  expectIndexedPages(text, replacement, offsets);
  const auto next = std::upper_bound(offsets.begin(), offsets.end(), anchor);
  const size_t page = std::max<size_t>(1, next - offsets.begin()) - 1;
  EXPECT_LE(offsets[page], anchor);
  EXPECT_GT(offsets[page + 1], anchor);
}

TEST_F(TxtPageIndexTest, LegacyHomeProgressUsesSavedPageAndRejectsSourceMismatch) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  putLegacyIndex(legacy);
  putLegacyProgress(5);
  uint16_t page = 0;
  EXPECT_EQ(txtlayout::loadLegacyProgressPageCount("/cache", text.size(), page), legacy.offsets.size());
  EXPECT_EQ(page, 5);
  EXPECT_FALSE(txtlayout::loadLegacyProgressPageCount("/cache", text.size() + 1, page).has_value());
}

TEST_F(TxtPageIndexTest, HomeNeverPairsModernZeroProgressWithARetainedLegacyCountAfterWideLayout) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  const auto wide = indexText(text, 20, 10, 1);
  ASSERT_NE(legacy.offsets.size(), wide.offsets.size());
  putLegacyIndex(legacy);
  putLegacyProgress(0);
  const auto original = Storage.bytes("/cache/index.bin");
  const auto migrated = txtlayout::migrateLegacyProgress("/cache", text.size());
  ASSERT_TRUE(migrated.replacementSafe);
  ASSERT_EQ(migrated.offset, 0U);
  ASSERT_TRUE(txtlayout::savePageIndex("/cache/index.wide.bin", wide.spec, wide.offsets, migrated.replacementSafe));
  ASSERT_TRUE(txtlayout::saveProgressRecord("/cache", 0, 0));

  uint16_t page = 123;
  EXPECT_FALSE(txtlayout::loadLegacyProgressPageCount("/cache", text.size(), page).has_value());
  EXPECT_EQ(page, 0);
  EXPECT_EQ(Storage.bytes("/cache/progress.bin"), (std::vector<uint8_t>{0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(Storage.bytes("/cache/index.bin"), original);
  EXPECT_EQ(txtlayout::readLegacyCacheOffset("/cache", text.size(), 0), 0U);
  EXPECT_EQ(txtlayout::readPageIndexOffset("/cache/index.wide.bin", wide.spec, 0), 0U);
}

TEST_F(TxtPageIndexTest, HomePageOnlyLegacyProgressStillUsesTheOriginalBaseWithWideCachePresent) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  const auto wide = indexText(text, 20, 10, 1);
  putLegacyIndex(legacy);
  ASSERT_TRUE(txtlayout::savePageIndex("/cache/index.wide.bin", wide.spec, wide.offsets, true));
  putLegacyProgress(5);
  uint16_t page = 0;
  EXPECT_EQ(txtlayout::loadLegacyProgressPageCount("/cache", text.size(), page), legacy.offsets.size());
  EXPECT_EQ(page, 5);
  EXPECT_EQ(txtlayout::readLegacyCacheOffset("/cache", text.size(), page), legacy.offsets[page]);
  ASSERT_TRUE(Storage.remove("/cache/index.bin"));
  EXPECT_FALSE(txtlayout::loadLegacyProgressPageCount("/cache", text.size(), page).has_value());
}

TEST_F(TxtPageIndexTest, HomeModernProgressRequiresAnExactLegacyAnchorWithoutWideEvidence) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  const auto wide = indexText(text, 20, 10, 1);
  ASSERT_NE(legacy.offsets[5], wide.offsets[5]);
  putLegacyIndex(legacy);
  ASSERT_TRUE(txtlayout::saveProgressRecord("/cache", 5, legacy.offsets[5]));
  uint16_t page = 0;
  EXPECT_EQ(txtlayout::loadLegacyProgressPageCount("/cache", text.size(), page), legacy.offsets.size());
  EXPECT_EQ(page, 5);
  ASSERT_TRUE(txtlayout::saveProgressRecord("/cache", 5, wide.offsets[5]));
  EXPECT_FALSE(txtlayout::loadLegacyProgressPageCount("/cache", text.size(), page).has_value());
}

TEST_F(TxtPageIndexTest, HomeRejectsTruncatedLegacyProgressAndIndexFiles) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  putLegacyIndex(legacy);
  Storage.put("/cache/progress.bin", {0, 0, 0});
  uint16_t page = 0;
  EXPECT_FALSE(txtlayout::loadLegacyProgressPageCount("/cache", text.size(), page).has_value());
  putLegacyProgress(5);
  auto truncated = Storage.bytes("/cache/index.bin");
  truncated.resize(txtlayout::PAGE_INDEX_HEADER_BYTES + sizeof(uint32_t));
  Storage.put("/cache/index.bin", std::move(truncated));
  EXPECT_FALSE(txtlayout::loadLegacyProgressPageCount("/cache", text.size(), page).has_value());
}

TEST_F(TxtPageIndexTest, WideBackupOrTemporaryEvidenceAlsoMakesModernLegacyDenominatorsUnknown) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  const auto wide = indexText(text, 20, 10, 1);
  for (const char* suffix : {".bak", ".tmp"}) {
    Storage.reset();
    putLegacyIndex(legacy);
    ASSERT_TRUE(txtlayout::savePageIndex("/cache/index.wide.bin", wide.spec, wide.offsets, true));
    const std::string interrupted = std::string("/cache/index.wide.bin") + suffix;
    ASSERT_TRUE(Storage.rename("/cache/index.wide.bin", interrupted.c_str()));
    ASSERT_TRUE(txtlayout::saveProgressRecord("/cache", 0, 0));
    uint16_t page = 123;
    EXPECT_FALSE(txtlayout::loadLegacyProgressPageCount("/cache", text.size(), page).has_value());
    EXPECT_EQ(page, 0);
  }
}

TEST_F(TxtPageIndexTest, UnreadableLegacySourceIsNotOverwrittenBeforeItsByteAnchorCanBeMigrated) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  const auto replacement = indexText(text, 9, 30, 3);
  putLegacyIndex(legacy);
  putLegacyProgress(5);
  auto damaged = Storage.bytes("/cache/index.bin");
  damaged.resize(txtlayout::PAGE_INDEX_HEADER_BYTES + sizeof(uint32_t));
  Storage.put("/cache/index.bin", damaged);
  const auto migration = txtlayout::migrateLegacyProgress("/cache", text.size());
  EXPECT_FALSE(migration.offset.has_value());
  EXPECT_FALSE(migration.replacementSafe);
  EXPECT_FALSE(
      txtlayout::savePageIndex("/cache/index.bin", replacement.spec, replacement.offsets, migration.replacementSafe));
  EXPECT_EQ(Storage.bytes("/cache/index.bin"), damaged);
}

TEST_F(TxtPageIndexTest, InterruptedFiveByteMigrationRetainsTheOriginalAnchorAndSourceUntilCommit) {
  const auto text = rotationText();
  const auto legacy = indexText(text, 8, 30, 3);
  const auto replacement = indexText(text, 9, 30, 3);
  putLegacyIndex(legacy);
  putLegacyProgress(5);
  const auto original = Storage.bytes("/cache/index.bin");
  Storage.failWritesAt("/cache/progress.bin", 5);
  const auto interrupted = txtlayout::migrateLegacyProgress("/cache", text.size());
  EXPECT_FALSE(interrupted.replacementSafe);
  EXPECT_EQ(interrupted.offset, legacy.offsets[5]);
  ASSERT_EQ(Storage.bytes("/cache/progress.bin").size(), 5U);
  EXPECT_FALSE(
      txtlayout::savePageIndex("/cache/index.bin", replacement.spec, replacement.offsets, interrupted.replacementSafe));
  EXPECT_EQ(Storage.bytes("/cache/index.bin"), original);

  Storage.failWritesAt("/cache/progress.bin", std::numeric_limits<size_t>::max());
  const auto resumed = txtlayout::migrateLegacyProgress("/cache", text.size());
  EXPECT_TRUE(resumed.replacementSafe);
  EXPECT_EQ(resumed.offset, legacy.offsets[5]);
  EXPECT_EQ(txtlayout::readIndexUint32(Storage.bytes("/cache/progress.bin").data() + 2), legacy.offsets[5]);
  EXPECT_EQ(Storage.bytes("/cache/index.bin"), original);
}

TEST_F(TxtPageIndexTest, SourceValidationNeverAliasesAFullSizeToItsTruncatedOnDiskValue) {
  if constexpr (sizeof(size_t) > sizeof(uint32_t)) {
    const auto text = rotationText();
    const auto narrow = indexText(text, 8, 30, 3);
    ASSERT_TRUE(txtlayout::savePageIndex("/cache/index.bin", narrow.spec, narrow.offsets, true));
    ASSERT_TRUE(txtlayout::saveProgressRecord("/cache", 5, narrow.offsets[5]));
    auto differentSource = narrow.spec;
    differentSource.sourceSize += static_cast<size_t>(uint64_t{1} << 32);
    std::vector<size_t> offsets;
    EXPECT_FALSE(txtlayout::loadPageIndex("/cache/index.bin", differentSource, offsets));
    EXPECT_FALSE(txtlayout::readPageIndexOffset("/cache/index.bin", differentSource, 5).has_value());
  }
}

TEST_F(TxtPageIndexTest, ModernProgressPassesItsByteAnchorThroughWithoutMigrationWritesOrDuplicateReads) {
  const auto text = rotationText();
  const auto wide = indexText(text, 20, 10, 1);
  ASSERT_TRUE(txtlayout::saveProgressRecord("/cache", 5, wide.offsets[5]));
  const size_t reads = Storage.reads("/cache/progress.bin");
  const size_t writes = Storage.writeOpens;
  const auto restored = txtlayout::migrateLegacyProgress("/cache", text.size());
  EXPECT_TRUE(restored.replacementSafe);
  EXPECT_EQ(restored.offset, wide.offsets[5]);
  EXPECT_EQ(Storage.reads("/cache/progress.bin") - reads, 1U);
  EXPECT_EQ(Storage.writeOpens, writes);
  EXPECT_EQ(txtlayout::preferredRestoreOffset(700U, 600U, restored.offset), 700U);
}
