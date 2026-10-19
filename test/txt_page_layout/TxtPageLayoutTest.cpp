#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
