#include <gtest/gtest.h>

#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "LibraryIndexFile.h"

namespace library {

std::string joinLibraryPath(const std::string_view folder, const std::string_view name) {
  return std::string(folder) + "/" + std::string(name);
}

}  // namespace library

namespace {

std::vector<uint8_t> makeBlob(const uint64_t pathHash, const std::initializer_list<uint8_t> fields) {
  std::vector<uint8_t> blob(sizeof(pathHash) + fields.size());
  std::memcpy(blob.data(), &pathHash, sizeof(pathHash));
  std::copy(fields.begin(), fields.end(), blob.begin() + sizeof(pathHash));
  return blob;
}

struct MetadataIndexFixture {
  library::ClixHeader header{};
  library::ClixRecord record{};
  std::vector<uint8_t> bytes;
};

MetadataIndexFixture makeMetadataIndex(const uint8_t version, const std::string_view name,
                                       const std::initializer_list<std::string_view> fields,
                                       const uint32_t position = library::CLIX_UNKNOWN_SERIES_POSITION) {
  auto blob = makeBlob(0x0123456789ABCDEFULL, {});
  blob.insert(blob.end(), name.begin(), name.end());
  for (const auto field : fields) {
    blob.push_back(static_cast<uint8_t>(field.size()));
    blob.insert(blob.end(), field.begin(), field.end());
  }
  if (version >= 6) {
    const size_t at = blob.size();
    blob.resize(at + sizeof(position));
    std::memcpy(blob.data() + at, &position, sizeof(position));
  }

  MetadataIndexFixture fixture;
  auto& header = fixture.header;
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = version;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  library::layoutSections(header, 0, blob.size());
  fixture.record.nameLen = static_cast<uint8_t>(name.size());
  fixture.bytes.resize(header.selfSize, 0);
  std::memcpy(fixture.bytes.data(), &header, sizeof(header));
  std::memcpy(fixture.bytes.data() + library::recordOffset(header, 0), &fixture.record, sizeof(fixture.record));
  std::memcpy(fixture.bytes.data() + header.nameStart, blob.data(), blob.size());
  return fixture;
}

}  // namespace

TEST(LibraryIndexFile, MissingIndexDoesNotCloseAnUninitializedHandle) {
  Storage.clearFile();
  HalFile::resetInvalidCloseCount();

  {
    library::LibraryIndexFile index;
    EXPECT_FALSE(index.open("/missing.clx"));
  }

  EXPECT_EQ(HalFile::invalidCloseCount(), 0);
}

TEST(LibraryIndexFile, ReadsEveryStoredOrderInBothDirections) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 3;
  library::layoutSections(header, 0, 0);

  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  const uint16_t authorOrder[] = {2, 0, 1};
  const uint16_t firstNameOrder[] = {0, 2, 1};
  const uint16_t arrivalOrder[] = {1, 2, 0};
  const uint16_t seriesOrder[] = {1, 0, 2};
  const uint16_t genreOrder[] = {2, 1, 0};
  std::memcpy(bytes.data() + library::authorOrderOffset(header, 0), authorOrder, sizeof(authorOrder));
  std::memcpy(bytes.data() + library::firstNameOrderOffset(header, 0), firstNameOrder, sizeof(firstNameOrder));
  std::memcpy(bytes.data() + library::arrivalOrderOffset(header, 0), arrivalOrder, sizeof(arrivalOrder));
  std::memcpy(bytes.data() + library::seriesOrderOffset(header, 0), seriesOrder, sizeof(seriesOrder));
  std::memcpy(bytes.data() + library::genreOrderOffset(header, 0), genreOrder, sizeof(genreOrder));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));

  const auto expectOrder = [&](const library::SortOrder order, const uint16_t a, const uint16_t b, const uint16_t c) {
    EXPECT_EQ(index.ordinalForRow(order, 0), a);
    EXPECT_EQ(index.ordinalForRow(order, 1), b);
    EXPECT_EQ(index.ordinalForRow(order, 2), c);
    EXPECT_EQ(index.ordinalForRow(order, 3), 0xFFFF);
  };
  expectOrder(library::SortOrder::RecentAsc, 1, 2, 0);
  expectOrder(library::SortOrder::RecentDesc, 0, 2, 1);
  expectOrder(library::SortOrder::TitleAsc, 0, 1, 2);
  expectOrder(library::SortOrder::TitleDesc, 2, 1, 0);
  expectOrder(library::SortOrder::AuthorAsc, 2, 0, 1);
  expectOrder(library::SortOrder::AuthorDesc, 1, 0, 2);
  expectOrder(library::SortOrder::AuthorFirstAsc, 0, 2, 1);
  expectOrder(library::SortOrder::AuthorFirstDesc, 1, 2, 0);
  expectOrder(library::SortOrder::SeriesAsc, 1, 0, 2);
  expectOrder(library::SortOrder::SeriesDesc, 2, 0, 1);
  expectOrder(library::SortOrder::GenreAsc, 2, 1, 0);
  expectOrder(library::SortOrder::GenreDesc, 0, 1, 2);
}

TEST(LibraryIndexFile, ResolvesRecentRowsByIdentity) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 3;
  // One 8-byte path hash blob per record.
  library::layoutSections(header, 0, 3 * sizeof(uint64_t));
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));

  // Ordinals 0 and 2 share a size, so only the hash can tell them apart.
  constexpr uint64_t HASHES[] = {11, 22, 33};
  constexpr uint32_t SIZES[] = {100, 200, 100};
  for (uint16_t ordinal = 0; ordinal < 3; ordinal++) {
    library::ClixRecord record{};
    record.fileSize = SIZES[ordinal];
    record.nameOff = ordinal * sizeof(uint64_t);
    std::memcpy(bytes.data() + library::recordOffset(header, ordinal), &record, sizeof(record));
    std::memcpy(bytes.data() + header.nameStart + record.nameOff, &HASHES[ordinal], sizeof(uint64_t));
  }
  const uint16_t arrivalOrder[] = {1, 2, 0};
  std::memcpy(bytes.data() + library::arrivalOrderOffset(header, 0), arrivalOrder, sizeof(arrivalOrder));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  const library::BookIdentity books[] = {
      {HASHES[0], SIZES[0]},  // ordinal 0 -> ascending row 2
      {HASHES[2], SIZES[2]},  // same size as ordinal 0, hash picks ordinal 2 -> row 1
      {99, SIZES[0]},         // size matches, hash does not: absent
      {HASHES[1], 999},       // hash matches, size does not: absent
      {HASHES[1], 0},         // size unknown: the hash alone matches -> row 0
  };
  uint16_t rows[5] = {};
  ASSERT_TRUE(index.recentRowsFor(books, 5, rows));
  EXPECT_EQ(rows[0], 2);
  EXPECT_EQ(rows[1], 1);
  EXPECT_EQ(rows[2], 0xFFFF);
  EXPECT_EQ(rows[3], 0xFFFF);
  EXPECT_EQ(rows[4], 0);
}

TEST(LibraryIndexFile, RejectsInvalidPermutationOrdinal) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  library::layoutSections(header, 0, 0);
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  const uint16_t invalid = 1;
  std::memcpy(bytes.data() + library::authorOrderOffset(header, 0), &invalid, sizeof(invalid));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_EQ(index.ordinalForRow(library::SortOrder::AuthorAsc, 0), 0xFFFF);
}

TEST(LibraryIndexFile, ReadsPathHashAndEveryPublicBlobField) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  const uint8_t folder[] = {6, '/', 'b', 'o', 'o', 'k', 's'};
  constexpr uint64_t PATH_HASH = 0x0123456789ABCDEFULL;
  const auto blob =
      makeBlob(PATH_HASH, {'x', 1, 'a', 1, 't', 8, 'O', 'r', 'i', 'g', 'i', 'n', 'a', 'l', 1, 's', 1, 'g', 0, 0, 0, 0});
  header.folderCount = 1;
  library::layoutSections(header, sizeof(folder), blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.folderStart, folder, sizeof(folder));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 1;
  Storage.setFile("/library.clx", bytes);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  uint64_t pathHash = 0;
  ASSERT_TRUE(index.readPathHash(record, pathHash));
  EXPECT_EQ(pathHash, PATH_HASH);
  std::string name;
  ASSERT_TRUE(index.readName(record, name));
  EXPECT_EQ(name, "x");
  std::string author;
  ASSERT_TRUE(index.readAuthor(record, author));
  EXPECT_EQ(author, "a");
  std::string title;
  ASSERT_TRUE(index.readTitle(record, title));
  EXPECT_EQ(title, "t");
  ASSERT_TRUE(index.readSourceAuthor(record, author));
  EXPECT_EQ(author, "Original");
  std::string series;
  ASSERT_TRUE(index.readSeries(record, series));
  EXPECT_EQ(series, "s");
  std::string genre;
  ASSERT_TRUE(index.readGenre(record, genre));
  EXPECT_EQ(genre, "g");
  uint32_t position = library::CLIX_UNKNOWN_SERIES_POSITION;
  ASSERT_TRUE(index.readSeriesPosition(record, position));
  EXPECT_EQ(position, 0u);
  ASSERT_TRUE(index.readSourceMetadata(record, title, author, series, genre, position));
  EXPECT_EQ(title, "t");
  EXPECT_EQ(author, "Original");
  EXPECT_EQ(series, "s");
  EXPECT_EQ(genre, "g");
  EXPECT_EQ(position, 0u);
  std::string path;
  ASSERT_TRUE(index.readPath(record, path));
  EXPECT_EQ(path, "/books/x");
  index.close();

  bytes[header.nameStart + sizeof(PATH_HASH) + 5] = 255;
  Storage.setFile("/library.clx", std::move(bytes));
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_FALSE(index.readSourceAuthor(record, author));
}

TEST(LibraryIndexFile, RejectsTruncatedAndOverflowingPathHashes) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  library::layoutSections(header, 0, sizeof(uint64_t) - 1);
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  library::ClixRecord record{};
  uint64_t hash = 1;
  EXPECT_FALSE(index.readPathHash(record, hash));
  EXPECT_EQ(hash, 0u);
  EXPECT_TRUE(index.ioFailed());

  record.nameOff = UINT32_MAX;
  EXPECT_FALSE(index.readPathHash(record, hash));
}

TEST(LibraryIndexFile, RejectsFolderRecordBeyondFolderBlob) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  const uint8_t folder[] = {5, '/'};
  const auto blob = makeBlob(1, {'x', 0, 0, 0});
  library::layoutSections(header, sizeof(folder), blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.folderStart, folder, sizeof(folder));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 1;
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string path;
  EXPECT_FALSE(index.readPath(record, path));
}

TEST(LibraryIndexFile, FolderCheckpointsResolvePathsAcrossBuckets) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  header.folderCount = 130;
  std::vector<uint8_t> folders;
  for (unsigned i = 0; i < header.folderCount; ++i) {
    const std::string dir = "/b" + std::to_string(i);
    folders.push_back(static_cast<uint8_t>(dir.size()));
    folders.insert(folders.end(), dir.begin(), dir.end());
  }
  const auto blob = makeBlob(1, {'x'});
  library::layoutSections(header, folders.size(), blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.folderStart, folders.data(), folders.size());
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  uint32_t offsets[library::LibraryIndexFile::FOLDER_CHECKPOINT_COUNT]{};
  uint16_t stride = 0;
  ASSERT_TRUE(index.buildFolderCheckpoints(offsets, stride));
  EXPECT_EQ(stride, 2);
  library::ClixRecord record{};
  record.nameLen = 1;
  std::string path;
  for (const uint16_t folderId : {uint16_t{0}, uint16_t{1}, uint16_t{128}, uint16_t{129}}) {
    record.folderId = folderId;
    ASSERT_TRUE(index.readPath(record, path, offsets, stride));
    EXPECT_EQ(path, "/b" + std::to_string(folderId) + "/x");
  }
}

TEST(LibraryIndexFile, DisplayTextAcceptsMissingMetadataAndRejectsMalformedFields) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  const auto blob = makeBlob(123, {'n', 'o', 't', 'e', '.', 't', 'x', 't', 0, 0, 0});
  library::layoutSections(header, 0, blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 8;
  Storage.setFile("/library.clx", bytes);
  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string title = "previous title";
  std::string author = "previous author";
  ASSERT_TRUE(index.readDisplayText(record, title, author));
  EXPECT_EQ(title, "note");
  EXPECT_TRUE(author.empty());
  index.close();
  bytes[header.nameStart + sizeof(uint64_t) + record.nameLen] = 255;
  Storage.setFile("/library.clx", std::move(bytes));
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_FALSE(index.readDisplayText(record, title, author));
}

TEST(LibraryIndexFile, SourceMetadataMatchesIndividualFieldsAcrossLegacyVersions) {
  constexpr uint32_t POSITION = 0x89ABCDEFu;
  for (uint8_t version = 2; version <= library::CLIX_FORMAT_VERSION; ++version) {
    SCOPED_TRACE(static_cast<unsigned>(version));
    for (const bool empty : {false, true}) {
      SCOPED_TRACE(empty);
      const std::string_view expectedTitle = empty ? std::string_view{} : std::string_view("A\0Title", 7);
      const std::string_view expectedAuthor = empty ? "" : "李白 / Jules Verne";
      const std::string_view expectedSeries = empty ? "" : "Cycle II";
      const std::string_view expectedGenre = empty ? "" : "冒険";
      auto fixture =
          version >= 3
              ? makeMetadataIndex(version, "filename.epub",
                                  {"Display", expectedTitle, expectedAuthor, expectedSeries, expectedGenre}, POSITION)
              : makeMetadataIndex(version, "filename.epub", {"Display", expectedTitle, expectedAuthor}, POSITION);
      Storage.setFile("/library.clx", fixture.bytes);
      library::LibraryIndexFile index;
      ASSERT_TRUE(index.openForReconciliation("/library.clx"));
      library::ClixRecord record{};
      ASSERT_TRUE(index.readRecord(0, record));

      std::string title = "stale title";
      std::string author = "stale author";
      std::string series = "stale series";
      std::string genre = "stale genre";
      uint32_t position = 0;
      ASSERT_TRUE(index.readSourceMetadata(record, title, author, series, genre, position));
      EXPECT_EQ(title, expectedTitle);
      EXPECT_EQ(author, expectedAuthor);
      EXPECT_EQ(series, version >= 3 ? expectedSeries : std::string_view{});
      EXPECT_EQ(genre, version >= 3 ? expectedGenre : std::string_view{});
      EXPECT_EQ(position, version >= 6 ? POSITION : library::CLIX_UNKNOWN_SERIES_POSITION);

      std::string individual;
      EXPECT_EQ(index.readTitle(record, individual), !expectedTitle.empty());
      EXPECT_EQ(individual, title);
      ASSERT_TRUE(index.readSourceAuthor(record, individual));
      EXPECT_EQ(individual, author);
      if (version >= 3) {
        ASSERT_TRUE(index.readSeries(record, individual));
        EXPECT_EQ(individual, series);
        ASSERT_TRUE(index.readGenre(record, individual));
        EXPECT_EQ(individual, genre);
      } else {
        EXPECT_FALSE(index.readSeries(record, individual));
        EXPECT_FALSE(index.readGenre(record, individual));
      }
      uint32_t individualPosition = 0;
      EXPECT_EQ(index.readSeriesPosition(record, individualPosition), version >= 6);
      EXPECT_EQ(individualPosition, position);

      ASSERT_TRUE(index.readDisplayText(record, title, author));
      EXPECT_EQ(title, empty ? std::string_view("filename") : expectedTitle);
      EXPECT_EQ(author, "Display");
      EXPECT_FALSE(index.ioFailed());
    }
  }
}

TEST(LibraryIndexFile, VersionOneRemainsUnavailableForReconciliation) {
  auto fixture = makeMetadataIndex(1, "book.epub", {"Display", "Title", "Source"});
  Storage.setFile("/library.clx", std::move(fixture.bytes));
  library::LibraryIndexFile index;
  EXPECT_FALSE(index.openForReconciliation("/library.clx"));
  EXPECT_EQ(index.validity(), library::ClixValidity::UnknownFormatVersion);
  std::string title, author, series, genre;
  uint32_t position = 0;
  EXPECT_FALSE(index.readSourceMetadata(fixture.record, title, author, series, genre, position));
  EXPECT_EQ(position, library::CLIX_UNKNOWN_SERIES_POSITION);
  EXPECT_FALSE(index.ioFailed());
}

TEST(LibraryIndexFile, SourceMetadataPreservesMaximumLengthAndNonTextBytes) {
  const std::string name(255, 'n');
  std::string field(255, 'x');
  field.front() = '\0';
  field.back() = static_cast<char>(0xFF);
  auto fixture = makeMetadataIndex(library::CLIX_FORMAT_VERSION, name, {field, field, field, field, field});
  Storage.setFile("/library.clx", std::move(fixture.bytes));
  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  library::ClixRecord record{};
  ASSERT_TRUE(index.readRecord(0, record));
  std::string title, author, series, genre;
  uint32_t position = 0;
  ASSERT_TRUE(index.readSourceMetadata(record, title, author, series, genre, position));
  EXPECT_EQ(title, field);
  EXPECT_EQ(author, field);
  EXPECT_EQ(series, field);
  EXPECT_EQ(genre, field);
  EXPECT_EQ(position, library::CLIX_UNKNOWN_SERIES_POSITION);
  ASSERT_TRUE(index.readDisplayText(record, title, author));
  EXPECT_EQ(title, field);
  EXPECT_EQ(author, field);
  ASSERT_TRUE(index.readName(record, title));
  EXPECT_EQ(title, name);
  EXPECT_FALSE(index.ioFailed());
}

TEST(LibraryIndexFile, SourceMetadataRejectsEveryTruncatedBlobBoundaryWithoutIoFailure) {
  const auto complete = makeMetadataIndex(library::CLIX_FORMAT_VERSION, "book.epub",
                                          {"Display", "Title", "Source", "Series", "Genre"}, 7);
  for (uint32_t blobBytes = 0; blobBytes < complete.header.nameLen; ++blobBytes) {
    SCOPED_TRACE(blobBytes);
    auto fixture = complete;
    library::layoutSections(fixture.header, 0, blobBytes);
    fixture.bytes.resize(fixture.header.selfSize);
    std::memcpy(fixture.bytes.data(), &fixture.header, sizeof(fixture.header));
    Storage.setFile("/library.clx", std::move(fixture.bytes));
    library::LibraryIndexFile index;
    ASSERT_TRUE(index.open("/library.clx"));
    std::string title, author, series, genre;
    uint32_t position = 0;
    EXPECT_FALSE(index.readSourceMetadata(fixture.record, title, author, series, genre, position));
    EXPECT_EQ(position, library::CLIX_UNKNOWN_SERIES_POSITION);
    EXPECT_FALSE(index.readSeriesPosition(fixture.record, position));
    EXPECT_FALSE(index.ioFailed());
  }
}

TEST(LibraryIndexFile, SourceMetadataRejectsMalformedPrefixesWithoutIoFailure) {
  const auto complete = makeMetadataIndex(library::CLIX_FORMAT_VERSION, "x", {"D", "T", "S", "R", "G"}, 7);
  const size_t firstPrefix = complete.header.nameStart + sizeof(uint64_t) + complete.record.nameLen;
  for (size_t field = 0; field < 5; ++field) {
    SCOPED_TRACE(field);
    auto bytes = complete.bytes;
    bytes[firstPrefix + 2 * field] = 255;
    Storage.setFile("/library.clx", std::move(bytes));
    library::LibraryIndexFile index;
    ASSERT_TRUE(index.open("/library.clx"));
    std::string title, author, series, genre;
    uint32_t position = 0;
    EXPECT_FALSE(index.readSourceMetadata(complete.record, title, author, series, genre, position));
    EXPECT_FALSE(index.readGenre(complete.record, genre));
    EXPECT_FALSE(index.readSeriesPosition(complete.record, position));
    if (field < 2) {
      EXPECT_FALSE(index.readDisplayText(complete.record, title, author));
    } else {
      ASSERT_TRUE(index.readDisplayText(complete.record, title, author));
      EXPECT_EQ(title, "T");
      EXPECT_EQ(author, "D");
    }
    EXPECT_FALSE(index.ioFailed());
  }
}

TEST(LibraryIndexFile, SourceMetadataRejectsOverflowingAndIncompleteNameRangesWithoutIoFailure) {
  auto fixture = makeMetadataIndex(library::CLIX_FORMAT_VERSION, "x", {"D", "T", "S", "R", "G"}, 7);
  Storage.setFile("/library.clx", std::move(fixture.bytes));
  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  for (const uint32_t offset :
       {UINT32_MAX, fixture.header.nameLen, fixture.header.nameLen - 7, fixture.header.nameLen - 8}) {
    SCOPED_TRACE(offset);
    auto record = fixture.record;
    record.nameOff = offset;
    std::string title, author, series, genre;
    uint32_t position = 0;
    EXPECT_FALSE(index.readSourceMetadata(record, title, author, series, genre, position));
    EXPECT_EQ(position, library::CLIX_UNKNOWN_SERIES_POSITION);
    EXPECT_FALSE(index.ioFailed());
  }
}

TEST(LibraryIndexFile, SourceMetadataObservesTruncationBetweenCallsAndKeepsIoFailureSticky) {
  const auto fixture = makeMetadataIndex(library::CLIX_FORMAT_VERSION, "book.epub", {"D", "T", "S", "R", "G"}, 7);
  const uint32_t firstPrefix = fixture.header.nameStart + sizeof(uint64_t) + fixture.record.nameLen;
  for (uint32_t cut = firstPrefix; cut < fixture.header.selfSize; ++cut) {
    SCOPED_TRACE(cut);
    Storage.setFile("/library.clx", fixture.bytes);
    library::LibraryIndexFile index;
    ASSERT_TRUE(index.open("/library.clx"));
    std::string title, author, series, genre;
    uint32_t position = 0;
    ASSERT_TRUE(index.readSourceMetadata(fixture.record, title, author, series, genre, position));
    EXPECT_FALSE(index.ioFailed());

    auto truncated = fixture.bytes;
    truncated.resize(cut);
    Storage.setFile("/library.clx", std::move(truncated));
    EXPECT_FALSE(index.readSourceMetadata(fixture.record, title, author, series, genre, position));
    EXPECT_TRUE(index.ioFailed());

    Storage.setFile("/library.clx", fixture.bytes);
    ASSERT_TRUE(index.readSourceMetadata(fixture.record, title, author, series, genre, position));
    EXPECT_EQ(title, "T");
    EXPECT_EQ(author, "S");
    EXPECT_EQ(series, "R");
    EXPECT_EQ(genre, "G");
    EXPECT_EQ(position, 7u);
    EXPECT_TRUE(index.ioFailed());
    index.close();
    ASSERT_TRUE(index.open("/library.clx"));
    EXPECT_FALSE(index.ioFailed());
  }
}

TEST(LibraryIndexFile, SeriesPositionRetainsZeroLengthNameCompatibility) {
  auto fixture = makeMetadataIndex(library::CLIX_FORMAT_VERSION, "", {"D", "T", "S", "R", "G"}, 0x7FC00000u);
  Storage.setFile("/library.clx", std::move(fixture.bytes));
  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  uint32_t position = 0;
  ASSERT_TRUE(index.readSeriesPosition(fixture.record, position));
  EXPECT_EQ(position, 0x7FC00000u);
  std::string title, author, series, genre;
  EXPECT_FALSE(index.readSourceMetadata(fixture.record, title, author, series, genre, position));
  EXPECT_EQ(position, library::CLIX_UNKNOWN_SERIES_POSITION);
  EXPECT_FALSE(index.ioFailed());
}

TEST(LibraryIndexFile, SourceMetadataKeepsGlobalBlobBoundsForTrailingPosition) {
  auto fixture = makeMetadataIndex(library::CLIX_FORMAT_VERSION, "x", {"D", "T", "S", "R", "G"});
  fixture.bytes.resize(fixture.bytes.size() - sizeof(uint32_t));
  const auto followingBlob = makeBlob(0x0123456789ABCDEFULL, {'y', 0, 0, 0});
  fixture.bytes.insert(fixture.bytes.end(), followingBlob.begin(), followingBlob.end());
  library::layoutSections(fixture.header, 0, fixture.bytes.size() - fixture.header.nameStart);
  std::memcpy(fixture.bytes.data(), &fixture.header, sizeof(fixture.header));
  uint32_t expectedPosition = 0;
  std::memcpy(&expectedPosition, followingBlob.data(), sizeof(expectedPosition));
  Storage.setFile("/library.clx", std::move(fixture.bytes));
  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string title, author, series, genre;
  uint32_t position = 0;
  ASSERT_TRUE(index.readSourceMetadata(fixture.record, title, author, series, genre, position));
  EXPECT_EQ(title, "T");
  EXPECT_EQ(author, "S");
  EXPECT_EQ(series, "R");
  EXPECT_EQ(genre, "G");
  EXPECT_EQ(position, expectedPosition);
  ASSERT_TRUE(index.readSeriesPosition(fixture.record, position));
  EXPECT_EQ(position, expectedPosition);
  EXPECT_FALSE(index.ioFailed());
}
