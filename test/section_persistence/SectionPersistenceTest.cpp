#include <gtest/gtest.h>

// Include standard headers before the class/private macros below: libstdc++
// templates declared with `class` do not compile if first parsed under them.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define class struct
#define private public
#include "Epub/Section.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

#include <GfxRenderer.h>

namespace {
constexpr uint8_t kFullVersion = 83;
constexpr uint8_t kPartialVersion = 0xC4;
constexpr uint8_t kPreviousFullVersion = 82;
constexpr uint8_t kPreviousPartialVersion = 0xC3;
constexpr uint8_t kOlderFullVersion = 79;
constexpr uint8_t kOlderPartialVersion = 0xF4;
constexpr uint8_t kEarlierFullVersion = 78;
constexpr uint8_t kEarlierPartialVersion = 0xF2;
constexpr uint8_t kLastReleaseFullVersion = 77;
constexpr uint8_t kLastReleasePartialVersion = 0xF3;
constexpr uint8_t kPreviousReleasePrepPartialVersion = 0x80;

ReaderRenderSpec renderSpec() {
  ReaderRenderSpec spec;
  spec.fontId = 3;
  spec.viewportWidth = 480;
  spec.viewportHeight = 800;
  spec.hyphenationEnabled = true;
  spec.wordSpacing = 2;
  return spec;
}

uint32_t readUint32At(const std::vector<uint8_t>& bytes, const size_t position) {
  return static_cast<uint32_t>(bytes.at(position)) | (static_cast<uint32_t>(bytes.at(position + 1)) << 8) |
         (static_cast<uint32_t>(bytes.at(position + 2)) << 16) | (static_cast<uint32_t>(bytes.at(position + 3)) << 24);
}

void writeUint32At(std::vector<uint8_t>& bytes, const size_t position, const uint32_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) bytes.at(position + i) = static_cast<uint8_t>(value >> (i * 8));
}

struct SectionHarness {
  Epub epub{"/books/test.epub", "/cache"};
  GfxRenderer renderer;
  Section section;
  ReaderRenderSpec spec = renderSpec();
  size_t serializedHeaderSize = 0;

  explicit SectionHarness(const char* cacheSuffix = "") : section(epub, 0, renderer, cacheSuffix) {}

  void begin(const std::vector<std::pair<std::string, uint16_t>>& anchors = {}) {
    ASSERT_TRUE(section.prepareBuild(spec));
    section.builtPageCount_ = 0;
    section.pageCount = section.partial_ ? section.partialPageCount_ : 0;
    section.buildComplete_ = false;
    section.protectedImageUnits_ = 0;
    auto context = makeUniqueNoThrow<Section::BuildContext>();
    ASSERT_NE(context, nullptr);
    context->tmpSectionPath = section.binTmpPath();
    context->parsePath = "/cache/test.html";
    context->parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
        epub, context->parsePath, renderer, spec.fontId, spec.lineCompression, spec.extraParagraphSpacing,
        spec.forceParagraphIndents, spec.paragraphAlignment, spec.viewportWidth, spec.viewportHeight,
        spec.hyphenationEnabled, spec.focusReadingEnabled, spec.guideReadingEnabled, spec.wordSpacing,
        [](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t) {}, spec.embeddedStyle, "", "",
        spec.imageRendering, std::vector<std::string>{}, nullptr, nullptr, spec.renderMode);
    ASSERT_NE(context->parser, nullptr);
    context->parser->anchorData = anchors;
    section.build_ = std::move(context);
    ASSERT_TRUE(Storage.openFileForWrite("TEST", section.build_->tmpSectionPath, section.file));
    ASSERT_TRUE(section.writeSectionFileHeader(spec));
    serializedHeaderSize = section.file.position();
  }

  void appendPages(const size_t count, const uint16_t paragraphStride = 3, const uint16_t listItemStride = 5,
                   const uint32_t visibleTextStride = 17) {
    for (size_t i = 0; i < count; ++i) {
      ASSERT_EQ(section.build_->pageIndex.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
      const uint32_t offset = section.onPageComplete(std::make_unique<Page>());
      ASSERT_NE(offset, 0U);
      section.build_->pageIndex.appendPrepared({offset, static_cast<uint16_t>(i * paragraphStride),
                                                static_cast<uint16_t>(i * listItemStride),
                                                static_cast<uint32_t>(i * visibleTextStride)});
    }
  }

  bool commit(const uint8_t version, const uint32_t consumed = 0, const uint32_t total = 0) {
    return section.commitBuildFile(version, consumed, total);
  }

  void finishSuccessfulCommit() {
    section.build_.reset();
    section.buildComplete_ = true;
  }
};

struct RotationHarness {
  SectionHarness narrow;
  SectionHarness wide;

  std::string activePath() const { return narrow.section.filePath; }
  std::string alternatePath() const { return activePath() + ".alt.bin"; }

  bool create() {
    narrow.begin({{"destination", 4}, {"ending", 4}});
    if (!narrow.section.build_) return false;
    narrow.appendPages(6);
    if (!narrow.commit(kFullVersion)) return false;
    narrow.finishSuccessfulCommit();
    std::swap(wide.spec.viewportWidth, wide.spec.viewportHeight);
    wide.begin({{"destination", 2}, {"ending", 2}});
    if (!wide.section.build_) return false;
    wide.appendPages(3, 6, 10, 34);
    if (!wide.commit(kFullVersion)) return false;
    wide.finishSuccessfulCommit();
    return true;
  }
};

class SectionPersistenceTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(SectionPersistenceTest, FullCommitReopensAndResolvesMetadataAcrossAChunkBoundary) {
  SectionHarness harness;
  harness.begin({{"chapter", 0}, {"boundary", 64}});
  harness.appendPages(65);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();

  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  EXPECT_FALSE(reopened.isPartial());
  EXPECT_EQ(reopened.pageCount, 65);
  EXPECT_EQ(reopened.findAnchor("boundary"), 64);
  EXPECT_EQ(reopened.getParagraphIndexForPage(63), 189);
  EXPECT_EQ(reopened.getParagraphIndexForPage(64), 192);
  EXPECT_EQ(reopened.getListItemIndexForPage(64), 320);
  EXPECT_EQ(reopened.getVisibleTextOffsetForPage(64), 1088U);
  EXPECT_EQ(reopened.getPageForParagraphIndex(192), 64);
  EXPECT_EQ(reopened.getPageForListItemIndex(320), 64);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1088), 64);
  EXPECT_NE(reopened.loadPage(64), nullptr);
}

TEST_F(SectionPersistenceTest, PartialCommitFiltersFutureAnchorsAndFallsBackBeyondTheLivePrefix) {
  SectionHarness harness;
  harness.begin({{"chapter", 0}, {"boundary", 64}, {"future", 65}});
  harness.appendPages(65);
  ASSERT_TRUE(harness.commit(kPartialVersion, 12345, 67890));
  harness.finishSuccessfulCommit();

  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  ASSERT_TRUE(reopened.isPartial());
  EXPECT_EQ(reopened.pageCount, 65);
  EXPECT_EQ(reopened.findAnchor("boundary"), 64);
  EXPECT_EQ(reopened.findAnchor("future"), std::nullopt);

  auto replay = makeUniqueNoThrow<Section::BuildContext>();
  ASSERT_NE(replay, nullptr);
  for (size_t i = 0; i < 10; ++i) {
    ASSERT_EQ(replay->pageIndex.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
    replay->pageIndex.appendPrepared({static_cast<uint32_t>(1000 + i), static_cast<uint16_t>(i * 3U),
                                      static_cast<uint16_t>(i * 5U), static_cast<uint32_t>(i * 17U)});
  }
  reopened.build_ = std::move(replay);
  reopened.builtPageCount_ = 10;

  EXPECT_EQ(reopened.getVisibleTextOffsetForPage(9), 153U);
  EXPECT_EQ(reopened.getVisibleTextOffsetForPage(64), 1088U);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1088), 64);
  EXPECT_EQ(reopened.getParagraphIndexForPage(64), 192);
}

TEST_F(SectionPersistenceTest, FailedCommitKeepsThePreviousReadableCache) {
  SectionHarness baseline;
  baseline.begin({{"chapter", 0}});
  baseline.appendPages(2);
  ASSERT_TRUE(baseline.commit(kFullVersion));
  baseline.finishSuccessfulCommit();
  const std::vector<uint8_t> previous = Storage.bytes(baseline.section.filePath);

  SectionHarness replacement;
  replacement.begin({{"chapter", 0}});
  replacement.appendPages(65);
  Storage.failWritesAt(replacement.section.build_->tmpSectionPath, replacement.section.file.position() + 4);
  EXPECT_FALSE(replacement.commit(kFullVersion));
  EXPECT_FALSE(Storage.exists(replacement.section.build_->tmpSectionPath.c_str()));
  ASSERT_TRUE(Storage.exists(replacement.section.filePath.c_str()));
  EXPECT_EQ(Storage.bytes(replacement.section.filePath), previous);
  replacement.section.build_.reset();
}

TEST_F(SectionPersistenceTest, RejectsCachesFromPreviousLayoutRevisions) {
  for (const uint8_t staleVersion :
       {kPreviousFullVersion, kPreviousPartialVersion, kOlderFullVersion, kOlderPartialVersion, kEarlierFullVersion,
        kEarlierPartialVersion, kLastReleaseFullVersion, kLastReleasePartialVersion,
        kPreviousReleasePrepPartialVersion}) {
    Storage.reset();
    SectionHarness harness;
    harness.begin();
    harness.appendPages(1);
    ASSERT_TRUE(harness.commit(staleVersion, 12345, 67890));
    harness.finishSuccessfulCommit();

    Section reopened(harness.epub, 0, harness.renderer);
    EXPECT_FALSE(reopened.loadSectionFile(harness.spec));
    EXPECT_TRUE(Storage.exists(harness.section.filePath.c_str()));
    EXPECT_FALSE(reopened.getCachedPageCount().has_value());
    SectionHarness replacement;
    replacement.begin();
    replacement.appendPages(2);
    ASSERT_TRUE(replacement.commit(kFullVersion));
    replacement.finishSuccessfulCommit();
    Section metadata(replacement.epub, 0, replacement.renderer);
    EXPECT_EQ(metadata.getCachedPageCount(), 2);
  }
}
TEST_F(SectionPersistenceTest, ReverseLookupsResolvePagesAcrossALongChapter) {
  SectionHarness harness;
  harness.begin();
  harness.appendPages(1025);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  for (const uint16_t page : {0, 31, 32, 63, 64, 1024}) {
    SCOPED_TRACE(page);
    EXPECT_EQ(reopened.getPageForParagraphIndex(page * 3), page);
    EXPECT_EQ(reopened.getPageForListItemIndex(page * 5), page);
    EXPECT_EQ(reopened.getPageForVisibleTextOffset(page * 17), page);
  }
  EXPECT_EQ(reopened.getPageForParagraphIndex(3073), 1024);
  EXPECT_EQ(reopened.getPageForListItemIndex(5121), 1024);
}

TEST_F(SectionPersistenceTest, IndexLookupsKeepTheFirstDuplicateAcrossChunkBoundaries) {
  SectionHarness harness;
  harness.begin();
  for (size_t i = 0; i < 66; ++i) {
    ASSERT_EQ(harness.section.build_->pageIndex.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
    const uint32_t position = harness.section.onPageComplete(std::make_unique<Page>());
    const uint16_t paragraph = i < 31 ? 0 : (i <= 64 ? 100 : 65534);
    const uint16_t listItem = i < 32 ? 0 : (i <= 64 ? 500 : 65534);
    harness.section.build_->pageIndex.appendPrepared({position, paragraph, listItem, static_cast<uint32_t>(i * 17)});
  }
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  EXPECT_EQ(reopened.getPageForParagraphIndex(0), 0);
  EXPECT_EQ(reopened.getPageForParagraphIndex(1), 31);
  EXPECT_EQ(reopened.getPageForParagraphIndex(100), 31);
  EXPECT_EQ(reopened.getPageForParagraphIndex(101), 65);
  EXPECT_EQ(reopened.getPageForParagraphIndex(65534), 65);
  EXPECT_EQ(reopened.getPageForParagraphIndex(65535), 65);
  EXPECT_EQ(reopened.getPageForListItemIndex(0), 0);
  EXPECT_EQ(reopened.getPageForListItemIndex(1), 32);
  EXPECT_EQ(reopened.getPageForListItemIndex(500), 32);
  EXPECT_EQ(reopened.getPageForListItemIndex(501), 65);
  EXPECT_EQ(reopened.getPageForListItemIndex(65534), 65);
  EXPECT_EQ(reopened.getPageForListItemIndex(65535), 65);
}

TEST_F(SectionPersistenceTest, MaximumIndexLutCountDoesNotWrapTheLastScanChunk) {
  SectionHarness harness;
  harness.begin();
  harness.appendPages(65535, 1, 1, 1);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  EXPECT_EQ(reopened.getPageForParagraphIndex(65534), 65534);
  EXPECT_EQ(reopened.getPageForParagraphIndex(65535), 65534);
  EXPECT_EQ(reopened.getPageForListItemIndex(65534), 65534);
  EXPECT_EQ(reopened.getPageForListItemIndex(65535), 65534);
}

TEST_F(SectionPersistenceTest, IndexLookupsRejectEveryTruncatedPrefixUntilTheirOwnLutIsComplete) {
  SectionHarness harness;
  harness.begin();
  harness.appendPages(65);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  const auto bytes = Storage.bytes(reopened.filePath);
  const size_t paragraph = readUint32At(bytes, harness.serializedHeaderSize - 3 * sizeof(uint32_t));
  const size_t listItem = readUint32At(bytes, harness.serializedHeaderSize - 2 * sizeof(uint32_t));
  const size_t paragraphEnd = paragraph + sizeof(uint16_t) + 65 * sizeof(uint16_t);
  const size_t listItemEnd = listItem + 65 * sizeof(uint16_t);
  for (size_t cut = 0; cut < bytes.size(); ++cut) {
    SCOPED_TRACE(cut);
    Storage.put(reopened.filePath, std::vector<uint8_t>(bytes.begin(), bytes.begin() + cut));
    EXPECT_EQ(reopened.getPageForParagraphIndex(192), cut >= paragraphEnd ? std::optional<uint16_t>{64} : std::nullopt);
    EXPECT_EQ(reopened.getPageForListItemIndex(320), cut >= listItemEnd ? std::optional<uint16_t>{64} : std::nullopt);
  }
}

TEST_F(SectionPersistenceTest, IndexShortReadsPreserveAnyCompleteFirstMatchBeforeTheFailure) {
  SectionHarness harness;
  harness.begin();
  harness.appendPages(65);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  const auto bytes = Storage.bytes(reopened.filePath);
  const size_t paragraphEntries =
      readUint32At(bytes, harness.serializedHeaderSize - 3 * sizeof(uint32_t)) + sizeof(uint16_t);
  const size_t listItemEntries = readUint32At(bytes, harness.serializedHeaderSize - 2 * sizeof(uint32_t));
  for (const uint16_t page : {0, 31, 32, 64}) {
    SCOPED_TRACE(page);
    for (size_t prefix = 0; prefix <= 65 * sizeof(uint16_t); ++prefix) {
      SCOPED_TRACE(prefix);
      const auto expected =
          prefix >= (static_cast<size_t>(page) + 1) * sizeof(uint16_t) ? std::optional<uint16_t>{page} : std::nullopt;
      Storage.failReadsAt(reopened.filePath, paragraphEntries + prefix);
      EXPECT_EQ(reopened.getPageForParagraphIndex(page * 3), expected);
      Storage.failReadsAt(reopened.filePath, listItemEntries + prefix);
      EXPECT_EQ(reopened.getPageForListItemIndex(page * 5), expected);
    }
  }
}

TEST_F(SectionPersistenceTest, IndexReadErrorsPreserveCompleteMatchesBeforeTheFailedSector) {
  SectionHarness harness;
  harness.begin();
  harness.appendPages(65);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  const auto bytes = Storage.bytes(reopened.filePath);
  const size_t paragraphEntries =
      readUint32At(bytes, harness.serializedHeaderSize - 3 * sizeof(uint32_t)) + sizeof(uint16_t);
  const size_t listItemEntries = readUint32At(bytes, harness.serializedHeaderSize - 2 * sizeof(uint32_t));
  for (const uint16_t page : {0, 30, 31, 32, 63, 64}) {
    SCOPED_TRACE(page);
    for (const size_t prefix : {0, 1, 2, 3, 62, 63, 64, 65, 66, 67, 128, 129, 130}) {
      SCOPED_TRACE(prefix);
      const auto expected =
          prefix >= (static_cast<size_t>(page) + 1) * sizeof(uint16_t) ? std::optional<uint16_t>{page} : std::nullopt;
      Storage.failReadsWithErrorAt(reopened.filePath, paragraphEntries + prefix);
      EXPECT_EQ(reopened.getPageForParagraphIndex(page * 3), expected);
      EXPECT_EQ(reopened.getPageForParagraphIndex(65535), prefix == 130 ? std::optional<uint16_t>{64} : std::nullopt);
      Storage.failReadsWithErrorAt(reopened.filePath, listItemEntries + prefix);
      EXPECT_EQ(reopened.getPageForListItemIndex(page * 5), expected);
      EXPECT_EQ(reopened.getPageForListItemIndex(65535), prefix == 130 ? std::optional<uint16_t>{64} : std::nullopt);
    }
  }
}

TEST_F(SectionPersistenceTest, IndexLookupsRejectExtremeOffsetsAndOversizedOrEmptyLuts) {
  SectionHarness harness;
  harness.begin();
  harness.appendPages(65);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  const auto original = Storage.bytes(reopened.filePath);
  const size_t paragraphField = harness.serializedHeaderSize - 3 * sizeof(uint32_t);
  const size_t listItemField = harness.serializedHeaderSize - 2 * sizeof(uint32_t);
  for (const uint32_t offset : {0U, static_cast<uint32_t>(original.size()), 0xFFFFFFFEU, 0xFFFFFFFFU}) {
    SCOPED_TRACE(offset);
    auto bytes = original;
    writeUint32At(bytes, paragraphField, offset);
    Storage.put(reopened.filePath, std::move(bytes));
    EXPECT_FALSE(reopened.getPageForParagraphIndex(0).has_value());
    EXPECT_FALSE(reopened.getPageForListItemIndex(0).has_value());
    bytes = original;
    writeUint32At(bytes, listItemField, offset);
    Storage.put(reopened.filePath, std::move(bytes));
    EXPECT_FALSE(reopened.getPageForListItemIndex(0).has_value());
    EXPECT_EQ(reopened.getPageForParagraphIndex(0), 0);
  }
  const size_t paragraph = readUint32At(original, paragraphField);
  for (const uint16_t count : {0, 65535}) {
    SCOPED_TRACE(count);
    auto bytes = original;
    bytes.at(paragraph) = static_cast<uint8_t>(count);
    bytes.at(paragraph + 1) = static_cast<uint8_t>(count >> 8);
    Storage.put(reopened.filePath, std::move(bytes));
    EXPECT_FALSE(reopened.getPageForParagraphIndex(0).has_value());
    EXPECT_FALSE(reopened.getPageForListItemIndex(0).has_value());
  }
}

TEST_F(SectionPersistenceTest, PositionLookupPreservesFirstAndLastDuplicateAcrossBatchBoundary) {
  SectionHarness harness;
  harness.begin();
  for (size_t i = 0; i < 66; ++i) {
    ASSERT_EQ(harness.section.build_->pageIndex.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
    const uint32_t position = harness.section.onPageComplete(std::make_unique<Page>());
    const uint32_t offset = i < 31 ? 0 : (i <= 64 ? 100 : 200);
    harness.section.build_->pageIndex.appendPrepared({position, 0, 0, offset});
  }
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(0, true), 0);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(0), 30);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(100, true), 31);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(100), 64);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(150, true), 64);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(999), 65);
}

TEST_F(SectionPersistenceTest, PartialPositionLookupRejectsOffsetsBeyondCommittedPages) {
  SectionHarness harness;
  harness.begin();
  harness.appendPages(65);
  ASSERT_TRUE(harness.commit(kPartialVersion, 12345, 67890));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1088), 64);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1089), std::nullopt);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1089, true), std::nullopt);
}

TEST_F(SectionPersistenceTest, AlternatingLayoutsReuseExactPagesAndKeepConstructorOnlyMetadataOnTheActiveLayout) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  const auto narrowBytes = Storage.bytes(fixture.alternatePath());
  const auto wideBytes = Storage.bytes(fixture.activePath());
  Section reader(fixture.narrow.epub, 0, fixture.narrow.renderer);
  for (int rotation = 0; rotation < 6; ++rotation) {
    const bool wide = rotation % 2 != 0;
    ASSERT_TRUE(reader.loadSectionFile(wide ? fixture.wide.spec : fixture.narrow.spec));
    const uint16_t count = wide ? 3 : 6;
    const uint16_t destination = wide ? 2 : 4;
    EXPECT_EQ(reader.pageCount, count);
    EXPECT_EQ(reader.findAnchor("destination"), destination);
    EXPECT_EQ(reader.getParagraphIndexForPage(destination), 12);
    EXPECT_EQ(reader.getListItemIndexForPage(destination), 20);
    EXPECT_EQ(reader.getVisibleTextOffsetForPage(destination), 68U);
    EXPECT_EQ(reader.getPageForVisibleTextOffset(68, true), destination);
    EXPECT_NE(reader.loadPage(destination), nullptr);

    // ProgressMapper and both sync activities intentionally query a fresh
    // Section without a render spec. They must see the activated layout.
    Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
    EXPECT_EQ(metadata.getCachedPageCount(), count);
    EXPECT_EQ(metadata.getPageForAnchor("destination"), destination);
    EXPECT_EQ(metadata.getPageForParagraphIndex(12), destination);
    EXPECT_EQ(metadata.getPageForListItemIndex(20), destination);
    EXPECT_EQ(metadata.getPageForVisibleTextOffset(68, true), destination);
    EXPECT_EQ(Storage.bytes(fixture.activePath()), wide ? wideBytes : narrowBytes);
    EXPECT_EQ(Storage.bytes(fixture.alternatePath()), wide ? narrowBytes : wideBytes);
  }
}

TEST_F(SectionPersistenceTest, EverySpecMismatchRejectsBothLayoutsWithoutDestroyingEitherBeforeCommit) {
  for (int field = 0; field < 14; ++field) {
    Storage.reset();
    RotationHarness fixture;
    ASSERT_TRUE(fixture.create());
    const auto active = Storage.bytes(fixture.activePath());
    const auto alternate = Storage.bytes(fixture.alternatePath());
    auto changed = fixture.wide.spec;
    switch (field) {
      case 0:
        ++changed.fontId;
        break;
      case 1:
        changed.lineCompression += 0.1f;
        break;
      case 2:
        changed.extraParagraphSpacing = !changed.extraParagraphSpacing;
        break;
      case 3:
        changed.forceParagraphIndents = !changed.forceParagraphIndents;
        break;
      case 4:
        ++changed.paragraphAlignment;
        break;
      case 5:
        --changed.viewportWidth;
        break;
      case 6:
        --changed.viewportHeight;
        break;
      case 7:
        changed.hyphenationEnabled = !changed.hyphenationEnabled;
        break;
      case 8:
        changed.embeddedStyle = !changed.embeddedStyle;
        break;
      case 9:
        ++changed.imageRendering;
        break;
      case 10:
        changed.focusReadingEnabled = !changed.focusReadingEnabled;
        break;
      case 11:
        changed.guideReadingEnabled = !changed.guideReadingEnabled;
        break;
      case 12:
        ++changed.wordSpacing;
        break;
      case 13:
        changed.renderMode = EpubRenderMode::Balanced;
        break;
    }
    Section reader(fixture.narrow.epub, 0, fixture.narrow.renderer);
    EXPECT_FALSE(reader.loadSectionFile(changed)) << field;
    EXPECT_FALSE(reader.getCachedPageCount().has_value());
    EXPECT_FALSE(reader.getPageForAnchor("destination").has_value());
    EXPECT_FALSE(reader.getPageForVisibleTextOffset(68, true).has_value());
    EXPECT_EQ(Storage.bytes(fixture.activePath()), active);
    EXPECT_EQ(Storage.bytes(fixture.alternatePath()), alternate);
    Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
    EXPECT_EQ(metadata.getCachedPageCount(), 3);
    EXPECT_EQ(metadata.getPageForAnchor("destination"), 2);
    ASSERT_TRUE(reader.loadSectionFile(fixture.narrow.spec));
    EXPECT_EQ(reader.getPageForAnchor("destination"), 4);
  }
}

TEST_F(SectionPersistenceTest, FailedAlternateActivationRestoresTheLastActiveSyncCoordinates) {
  for (const bool failBeforePreserve : {true, false}) {
    Storage.reset();
    RotationHarness fixture;
    ASSERT_TRUE(fixture.create());
    const auto active = Storage.bytes(fixture.activePath());
    const auto alternate = Storage.bytes(fixture.alternatePath());
    Storage.failRenameFrom = failBeforePreserve ? fixture.activePath() : fixture.alternatePath();
    Section reader(fixture.narrow.epub, 0, fixture.narrow.renderer);
    EXPECT_FALSE(reader.loadSectionFile(fixture.narrow.spec));
    EXPECT_EQ(Storage.bytes(fixture.activePath()), active);
    EXPECT_EQ(Storage.bytes(fixture.alternatePath()), alternate);
    EXPECT_FALSE(reader.getPageForAnchor("destination").has_value());
    Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
    EXPECT_EQ(metadata.getCachedPageCount(), 3);
    EXPECT_EQ(metadata.getPageForAnchor("destination"), 2);
    EXPECT_EQ(metadata.getPageForParagraphIndex(12), 2);
    EXPECT_EQ(metadata.getPageForListItemIndex(20), 2);
    EXPECT_EQ(metadata.getPageForVisibleTextOffset(68, true), 2);
    EXPECT_FALSE(Storage.exists((fixture.activePath() + ".bak").c_str()));
    ASSERT_TRUE(reader.loadSectionFile(fixture.narrow.spec));
    EXPECT_EQ(reader.getPageForAnchor("destination"), 4);
  }
}

TEST_F(SectionPersistenceTest, ConstructorRecoveryRollsBackBeforeActivationAndFinishesRetentionAfterActivation) {
  for (const bool activationCommitted : {false, true}) {
    Storage.reset();
    RotationHarness fixture;
    ASSERT_TRUE(fixture.create());
    const auto wideBytes = Storage.bytes(fixture.activePath());
    const auto narrowBytes = Storage.bytes(fixture.alternatePath());
    const auto backup = fixture.activePath() + ".bak";
    ASSERT_TRUE(Storage.rename(fixture.activePath().c_str(), backup.c_str()));
    if (activationCommitted) {
      ASSERT_TRUE(Storage.rename(fixture.alternatePath().c_str(), fixture.activePath().c_str()));
    }
    Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
    EXPECT_EQ(metadata.getCachedPageCount(), activationCommitted ? 6 : 3);
    EXPECT_EQ(metadata.getPageForAnchor("destination"), activationCommitted ? 4 : 2);
    EXPECT_EQ(metadata.getPageForVisibleTextOffset(68, true), activationCommitted ? 4 : 2);
    EXPECT_EQ(Storage.bytes(fixture.activePath()), activationCommitted ? narrowBytes : wideBytes);
    EXPECT_EQ(Storage.bytes(fixture.alternatePath()), activationCommitted ? wideBytes : narrowBytes);
    EXPECT_FALSE(Storage.exists(backup.c_str()));
  }
}

TEST_F(SectionPersistenceTest, WideOnlyActiveBackupIsRecoveredForConstructorOnlySyncReaders) {
  SectionHarness wide;
  std::swap(wide.spec.viewportWidth, wide.spec.viewportHeight);
  wide.begin({{"destination", 2}});
  wide.appendPages(3, 6, 10, 34);
  ASSERT_TRUE(wide.commit(kFullVersion));
  wide.finishSuccessfulCommit();
  const auto active = wide.section.filePath;
  const auto backup = active + ".bak";
  ASSERT_TRUE(Storage.rename(active.c_str(), backup.c_str()));
  Section metadata(wide.epub, 0, wide.renderer);
  EXPECT_EQ(metadata.getCachedPageCount(), 3);
  EXPECT_EQ(metadata.getPageForAnchor("destination"), 2);
  EXPECT_EQ(metadata.getPageForParagraphIndex(12), 2);
  EXPECT_EQ(metadata.getPageForListItemIndex(20), 2);
  EXPECT_EQ(metadata.getPageForVisibleTextOffset(68, true), 2);
  EXPECT_FALSE(Storage.exists(backup.c_str()));
}

TEST_F(SectionPersistenceTest, NewSpecBuildNeverReadsTheMismatchingActiveLayoutAndFailureKeepsBothCaches) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  const auto active = Storage.bytes(fixture.activePath());
  const auto alternate = Storage.bytes(fixture.alternatePath());
  SectionHarness replacement;
  replacement.spec = fixture.narrow.spec;
  ++replacement.spec.fontId;
  replacement.begin({{"new-layout", 0}});
  replacement.appendPages(2);
  EXPECT_FALSE(replacement.section.getCachedPageCount().has_value());
  EXPECT_FALSE(replacement.section.findAnchor("destination").has_value());
  EXPECT_FALSE(replacement.section.getPageForParagraphIndex(12).has_value());
  EXPECT_FALSE(replacement.section.getPageForListItemIndex(20).has_value());
  EXPECT_FALSE(replacement.section.getPageForVisibleTextOffset(68, true).has_value());
  EXPECT_FALSE(replacement.section.getParagraphIndexForPage(2).has_value());
  EXPECT_FALSE(replacement.section.getListItemIndexForPage(2).has_value());
  EXPECT_FALSE(replacement.section.getVisibleTextOffsetForPage(2).has_value());
  EXPECT_EQ(replacement.section.loadPage(2), nullptr);
  EXPECT_EQ(replacement.section.findAnchor("new-layout"), 0);
  EXPECT_NE(replacement.section.loadPage(1), nullptr);
  EXPECT_FALSE(replacement.section.loadSectionFile(fixture.wide.spec));
  EXPECT_FALSE(replacement.section.startBuild(fixture.wide.spec));
  EXPECT_FALSE(replacement.section.createSectionFile(fixture.wide.spec));
  EXPECT_FALSE(replacement.section.clearCache());
  Storage.failWritesAt(replacement.section.build_->tmpSectionPath, replacement.section.file.position() + 4);
  EXPECT_FALSE(replacement.commit(kFullVersion));
  replacement.section.abandonBuild();
  EXPECT_EQ(Storage.bytes(fixture.activePath()), active);
  EXPECT_EQ(Storage.bytes(fixture.alternatePath()), alternate);
  Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
  EXPECT_EQ(metadata.getCachedPageCount(), 3);
  EXPECT_EQ(metadata.getPageForAnchor("destination"), 2);
}

TEST_F(SectionPersistenceTest, PartialExtensionAndFullPromotionDoNotEvictTheOtherCompletedLayout) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  const auto narrowBytes = Storage.bytes(fixture.alternatePath());
  SectionHarness partial;
  partial.spec = fixture.wide.spec;
  partial.begin({{"destination", 2}});
  partial.appendPages(4, 6, 10, 34);
  ASSERT_TRUE(partial.commit(kPartialVersion, 100, 1000));
  partial.finishSuccessfulCommit();
  EXPECT_EQ(Storage.bytes(fixture.alternatePath()), narrowBytes);
  ASSERT_TRUE(partial.section.loadSectionFile(partial.spec));
  ASSERT_TRUE(partial.section.isPartial());
  partial.begin({{"destination", 2}, {"retained", 3}});
  partial.appendPages(5, 6, 10, 34);
  partial.section.suspendBuild();
  EXPECT_EQ(Storage.bytes(fixture.alternatePath()), narrowBytes);
  Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
  EXPECT_FALSE(metadata.getCachedPageCount().has_value());
  EXPECT_EQ(metadata.getPageForAnchor("retained"), 3);
  ASSERT_TRUE(partial.section.loadSectionFile(partial.spec));
  EXPECT_TRUE(partial.section.isPartial());
  EXPECT_EQ(partial.section.pageCount, 5);
  partial.begin({{"destination", 2}});
  partial.appendPages(6, 6, 10, 34);
  ASSERT_TRUE(partial.commit(kFullVersion));
  partial.finishSuccessfulCommit();
  EXPECT_EQ(Storage.bytes(fixture.alternatePath()), narrowBytes);
  Section reader(fixture.narrow.epub, 0, fixture.narrow.renderer);
  ASSERT_TRUE(reader.loadSectionFile(fixture.narrow.spec));
  EXPECT_EQ(reader.getCachedPageCount(), 6);
  EXPECT_EQ(reader.getPageForAnchor("destination"), 4);
  ASSERT_TRUE(reader.loadSectionFile(fixture.wide.spec));
  EXPECT_FALSE(reader.isPartial());
  EXPECT_EQ(reader.getCachedPageCount(), 6);
  EXPECT_EQ(reader.getPageForAnchor("destination"), 2);
}

TEST_F(SectionPersistenceTest, FailedArchiveAfterActivationKeepsNewActiveAndConstructorCompletesRetention) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  const auto oldActive = Storage.bytes(fixture.activePath());
  const auto desired = Storage.bytes(fixture.alternatePath());
  const auto backup = fixture.activePath() + ".bak";
  Storage.failRenameFrom = backup;
  Section reader(fixture.narrow.epub, 0, fixture.narrow.renderer);
  ASSERT_TRUE(reader.loadSectionFile(fixture.narrow.spec));
  EXPECT_EQ(reader.getPageForAnchor("destination"), 4);
  EXPECT_EQ(Storage.bytes(fixture.activePath()), desired);
  EXPECT_EQ(Storage.bytes(backup), oldActive);
  Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
  EXPECT_EQ(metadata.getCachedPageCount(), 6);
  EXPECT_EQ(metadata.getPageForAnchor("destination"), 4);
  EXPECT_EQ(Storage.bytes(fixture.alternatePath()), oldActive);
  EXPECT_FALSE(Storage.exists(backup.c_str()));
}

TEST_F(SectionPersistenceTest, ExplicitInvalidationRemovesBothLayoutsWithoutTouchingOtherSuffixes) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  SectionHarness preview("_preview");
  preview.spec = fixture.wide.spec;
  preview.begin({{"preview", 0}});
  preview.appendPages(1);
  ASSERT_TRUE(preview.commit(kFullVersion));
  preview.finishSuccessfulCommit();
  SectionHarness balanced("_balanced");
  balanced.spec = fixture.wide.spec;
  balanced.spec.renderMode = EpubRenderMode::Balanced;
  balanced.begin({{"balanced", 1}});
  balanced.appendPages(2);
  ASSERT_TRUE(balanced.commit(kFullVersion));
  balanced.finishSuccessfulCommit();
  const auto previewBytes = Storage.bytes(preview.section.filePath);
  const auto balancedBytes = Storage.bytes(balanced.section.filePath);
  ASSERT_TRUE(fixture.wide.section.clearCache());
  EXPECT_FALSE(Storage.exists(fixture.activePath().c_str()));
  EXPECT_FALSE(Storage.exists(fixture.alternatePath().c_str()));
  EXPECT_EQ(Storage.bytes(preview.section.filePath), previewBytes);
  EXPECT_EQ(Storage.bytes(balanced.section.filePath), balancedBytes);
  Section previewMetadata(preview.epub, 0, preview.renderer, "_preview");
  Section balancedMetadata(balanced.epub, 0, balanced.renderer, "_balanced");
  EXPECT_EQ(previewMetadata.getPageForAnchor("preview"), 0);
  EXPECT_EQ(balancedMetadata.getPageForAnchor("balanced"), 1);
}

TEST_F(SectionPersistenceTest, FailedNewSpecPromotionPreservesTheActiveAndAlternateSyncPassages) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  const auto active = Storage.bytes(fixture.activePath());
  const auto alternate = Storage.bytes(fixture.alternatePath());
  SectionHarness replacement;
  replacement.spec = fixture.narrow.spec;
  ++replacement.spec.fontId;
  replacement.begin({{"new-layout", 1}});
  replacement.appendPages(2);
  Storage.failRenameFrom = replacement.section.build_->tmpSectionPath;
  EXPECT_FALSE(replacement.commit(kFullVersion));
  replacement.section.abandonBuild();
  EXPECT_EQ(Storage.bytes(fixture.activePath()), active);
  EXPECT_EQ(Storage.bytes(fixture.alternatePath()), alternate);
  Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
  EXPECT_EQ(metadata.getCachedPageCount(), 3);
  EXPECT_EQ(metadata.getPageForAnchor("destination"), 2);
  EXPECT_EQ(metadata.getPageForParagraphIndex(12), 2);
  EXPECT_EQ(metadata.getPageForListItemIndex(20), 2);
  EXPECT_EQ(metadata.getPageForVisibleTextOffset(68, true), 2);
}

TEST_F(SectionPersistenceTest, SuccessfulNewSpecPromotionRetainsThePreviousActiveRatherThanAnOlderAlternate) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  const auto previousActive = Storage.bytes(fixture.activePath());
  SectionHarness replacement;
  replacement.spec = fixture.narrow.spec;
  ++replacement.spec.fontId;
  replacement.begin({{"new-layout", 1}});
  replacement.appendPages(2);
  ASSERT_TRUE(replacement.commit(kFullVersion));
  replacement.finishSuccessfulCommit();
  EXPECT_EQ(Storage.bytes(fixture.alternatePath()), previousActive);
  Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
  EXPECT_EQ(metadata.getCachedPageCount(), 2);
  EXPECT_EQ(metadata.getPageForAnchor("new-layout"), 1);
  Section reader(fixture.narrow.epub, 0, fixture.narrow.renderer);
  ASSERT_TRUE(reader.loadSectionFile(fixture.wide.spec));
  EXPECT_EQ(reader.getPageForAnchor("destination"), 2);
  ASSERT_TRUE(reader.loadSectionFile(replacement.spec));
  EXPECT_EQ(reader.getPageForAnchor("new-layout"), 1);
}

TEST_F(SectionPersistenceTest, RecoveryOfASameSpecReplacementDoesNotEvictTheOtherLayout) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  const auto oldActive = Storage.bytes(fixture.activePath());
  const auto alternate = Storage.bytes(fixture.alternatePath());
  SectionHarness replacement;
  replacement.spec = fixture.wide.spec;
  replacement.begin({{"destination", 2}});
  replacement.appendPages(5, 6, 10, 34);
  ASSERT_TRUE(replacement.commit(kFullVersion));
  replacement.finishSuccessfulCommit();
  // Power loss after the new A rename but before same-spec backup cleanup.
  const auto backup = fixture.activePath() + ".bak";
  Storage.put(backup, oldActive);
  Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
  EXPECT_EQ(metadata.getCachedPageCount(), 5);
  EXPECT_EQ(metadata.getPageForAnchor("destination"), 2);
  EXPECT_EQ(Storage.bytes(fixture.alternatePath()), alternate);
  EXPECT_FALSE(Storage.exists(backup.c_str()));
  Section reader(fixture.narrow.epub, 0, fixture.narrow.renderer);
  ASSERT_TRUE(reader.loadSectionFile(fixture.narrow.spec));
  EXPECT_EQ(reader.getCachedPageCount(), 6);
  EXPECT_EQ(reader.getPageForAnchor("destination"), 4);
}

TEST_F(SectionPersistenceTest, ObsoleteActiveAndBackupFormatsDoNotBlockCurrentLayoutRebuild) {
  SectionHarness obsolete;
  obsolete.begin({{"obsolete", 0}});
  obsolete.appendPages(2);
  ASSERT_TRUE(obsolete.commit(kPreviousFullVersion));
  obsolete.finishSuccessfulCommit();
  const auto backup = obsolete.section.filePath + ".bak";
  Storage.put(backup, Storage.bytes(obsolete.section.filePath));
  Section metadataBefore(obsolete.epub, 0, obsolete.renderer);
  EXPECT_FALSE(metadataBefore.getCachedPageCount().has_value());

  SectionHarness current;
  current.begin({{"current", 1}});
  current.appendPages(3);
  ASSERT_TRUE(current.commit(kFullVersion));
  current.finishSuccessfulCommit();
  Section metadataAfter(current.epub, 0, current.renderer);
  EXPECT_EQ(metadataAfter.getCachedPageCount(), 3);
  EXPECT_EQ(metadataAfter.getPageForAnchor("current"), 1);
  EXPECT_FALSE(metadataAfter.getPageForAnchor("obsolete").has_value());
  EXPECT_FALSE(Storage.exists(backup.c_str()));
  ASSERT_TRUE(current.section.loadSectionFile(current.spec));
  EXPECT_NE(current.section.loadPage(2), nullptr);
}

TEST_F(SectionPersistenceTest, ConstructorRestoresAValidBackupWhenTheActiveHeaderIsInvalid) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  const auto active = Storage.bytes(fixture.activePath());
  const auto alternate = Storage.bytes(fixture.alternatePath());
  const auto backup = fixture.activePath() + ".bak";
  Storage.put(backup, active);
  Storage.put(fixture.activePath(), {0, 1, 2, 3});
  Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
  EXPECT_EQ(metadata.getCachedPageCount(), 3);
  EXPECT_EQ(metadata.getPageForAnchor("destination"), 2);
  EXPECT_EQ(metadata.getPageForVisibleTextOffset(68, true), 2);
  EXPECT_EQ(Storage.bytes(fixture.activePath()), active);
  EXPECT_EQ(Storage.bytes(fixture.alternatePath()), alternate);
  EXPECT_FALSE(Storage.exists(backup.c_str()));
}

TEST_F(SectionPersistenceTest, TruncatedAlternateHeaderCannotActivateOrReportAnotherLayout) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  const auto active = Storage.bytes(fixture.activePath());
  auto truncated = Storage.bytes(fixture.alternatePath());
  // Retain pageCount and protected image units, but remove all five fixed
  // LUT offsets. The full-version scalar fields alone are not a full header.
  truncated.resize(fixture.narrow.serializedHeaderSize - 5 * sizeof(uint32_t));
  Storage.put(fixture.alternatePath(), truncated);

  Section reader(fixture.narrow.epub, 0, fixture.narrow.renderer);
  EXPECT_FALSE(reader.loadSectionFile(fixture.narrow.spec));
  EXPECT_EQ(reader.pageCount, 0);
  EXPECT_FALSE(reader.getCachedPageCount().has_value());
  EXPECT_FALSE(reader.getPageForAnchor("destination").has_value());
  EXPECT_EQ(Storage.bytes(fixture.activePath()), active);
  EXPECT_TRUE(Storage.exists(fixture.alternatePath().c_str()));
  if (Storage.exists(fixture.alternatePath().c_str())) {
    EXPECT_EQ(Storage.bytes(fixture.alternatePath()), truncated);
  }
  Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
  EXPECT_EQ(metadata.getCachedPageCount(), 3);
  EXPECT_EQ(metadata.getPageForAnchor("destination"), 2);
  EXPECT_EQ(metadata.getPageForVisibleTextOffset(68, true), 2);
}

TEST_F(SectionPersistenceTest, UnresolvedRecoveryCannotReplaceAValidatedAlternateBeforeActivation) {
  RotationHarness fixture;
  ASSERT_TRUE(fixture.create());
  const auto activeWide = Storage.bytes(fixture.activePath());
  const auto previousNarrow = Storage.bytes(fixture.alternatePath());
  SectionHarness wanted;
  wanted.spec = fixture.narrow.spec;
  ++wanted.spec.fontId;
  wanted.begin({{"wanted-layout", 3}});
  wanted.appendPages(4);
  ASSERT_TRUE(wanted.commit(kFullVersion));
  wanted.finishSuccessfulCommit();
  const auto wantedBytes = Storage.bytes(fixture.activePath());
  Storage.put(fixture.activePath(), activeWide);
  Storage.put(fixture.alternatePath(), wantedBytes);

  Section reader(fixture.narrow.epub, 0, fixture.narrow.renderer);
  // Deferred retention appears after construction. Its first recovery read
  // fails transiently; a later recovery must not replace an already validated B.
  const auto backup = fixture.activePath() + ".bak";
  Storage.put(backup, previousNarrow);
  Storage.failNextRead(fixture.activePath());
  EXPECT_FALSE(reader.loadSectionFile(wanted.spec));
  EXPECT_EQ(reader.pageCount, 0);
  EXPECT_FALSE(reader.getCachedPageCount().has_value());
  EXPECT_FALSE(reader.getPageForVisibleTextOffset(68, true).has_value());
  EXPECT_EQ(Storage.bytes(fixture.activePath()), activeWide);
  EXPECT_EQ(Storage.bytes(fixture.alternatePath()), wantedBytes);
  EXPECT_TRUE(Storage.exists(backup.c_str()));
  if (Storage.exists(backup.c_str())) {
    EXPECT_EQ(Storage.bytes(backup), previousNarrow);
  }
  Section metadata(fixture.narrow.epub, 0, fixture.narrow.renderer);
  EXPECT_EQ(metadata.getCachedPageCount(), 3);
  EXPECT_EQ(metadata.getPageForAnchor("destination"), 2);
  EXPECT_EQ(metadata.getPageForVisibleTextOffset(68, true), 2);
}

TEST_F(SectionPersistenceTest, IdleCommittedPageSpansPreserveReadingPositionAndParagraphAnchors) {
  for (const uint8_t version : {kFullVersion, kPartialVersion}) {
    SectionHarness harness;
    harness.begin({{"destination", 1}});
    harness.appendPages(3);
    ASSERT_TRUE(harness.commit(version, 150, 300));
    harness.finishSuccessfulCommit();
    Section reader(harness.epub, 0, harness.renderer);
    ASSERT_TRUE(reader.loadSectionFile(harness.spec));
    reader.currentPage = 1;
    const auto before = Storage.bytes(harness.section.filePath);
    for (const int page : {0, 1, 2}) {
      EXPECT_NE(reader.loadPageForIdle(page), nullptr);
      EXPECT_EQ(reader.currentPage, 1);
      EXPECT_EQ(reader.getParagraphIndexForPage(1), 3);
      EXPECT_EQ(reader.findAnchor("destination"), 1);
    }
    EXPECT_EQ(reader.loadPageForIdle(-1), nullptr);
    EXPECT_EQ(reader.loadPageForIdle(3), nullptr);
    EXPECT_EQ(Storage.bytes(harness.section.filePath), before);
  }
}

TEST_F(SectionPersistenceTest, IdlePreparationDeclinesActiveBuildPagesWithoutChangingTheReadableWatermark) {
  SectionHarness harness;
  harness.begin();
  harness.appendPages(2);
  harness.section.currentPage = 1;
  const auto before = Storage.bytes(harness.section.binTmpPath());
  EXPECT_EQ(harness.section.loadPageForIdle(0), nullptr);
  EXPECT_EQ(harness.section.currentPage, 1);
  EXPECT_EQ(harness.section.pageCount, 2);
  EXPECT_EQ(Storage.bytes(harness.section.binTmpPath()), before);
}

TEST_F(SectionPersistenceTest, IdleMalformedLutSpansDeclineWithoutCacheWritesOrPositionChanges) {
  SectionHarness harness;
  harness.begin({{"destination", 1}});
  harness.appendPages(3);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  const auto original = Storage.bytes(harness.section.filePath);
  const size_t lutField = harness.serializedHeaderSize - sizeof(uint32_t) * 5;
  const uint32_t lut = readUint32At(original, lutField);
  const size_t pageField = lut + sizeof(uint32_t);
  const uint32_t pageStart = readUint32At(original, pageField);
  const uint32_t nextPage = readUint32At(original, pageField + sizeof(uint32_t));
  const std::vector<std::pair<size_t, uint32_t>> corruptions = {{lutField, 0},
                                                                {lutField, UINT32_MAX},
                                                                {lutField, static_cast<uint32_t>(original.size() - 3)},
                                                                {pageField, 0},
                                                                {pageField, nextPage},
                                                                {pageField, UINT32_MAX},
                                                                {pageField + sizeof(uint32_t), 0},
                                                                {pageField + sizeof(uint32_t), pageStart},
                                                                {pageField + sizeof(uint32_t), lut + 1}};
  for (const auto& [offset, value] : corruptions) {
    Storage.put(harness.section.filePath, original);
    Section reader(harness.epub, 0, harness.renderer);
    ASSERT_TRUE(reader.loadSectionFile(harness.spec));
    reader.currentPage = 1;
    auto corrupt = original;
    writeUint32At(corrupt, offset, value);
    Storage.put(harness.section.filePath, corrupt);
    EXPECT_EQ(reader.loadPageForIdle(1), nullptr);
    EXPECT_EQ(reader.currentPage, 1);
    EXPECT_EQ(reader.pageCount, 3);
    EXPECT_EQ(Storage.bytes(harness.section.filePath), corrupt);
    Storage.put(harness.section.filePath, original);
    EXPECT_NE(reader.loadPage(1), nullptr);
    EXPECT_EQ(reader.getParagraphIndexForPage(1), 3);
    EXPECT_EQ(reader.findAnchor("destination"), 1);
  }
}

}  // namespace
