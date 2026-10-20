#include <ZipFile.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint32_t kUntouched = 0xDEADBEEF;

void put16(Bytes& bytes, const size_t offset, const uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(Bytes& bytes, const size_t offset, const uint32_t value) {
  for (size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

template <typename Container>
void append(Bytes& destination, const Container& source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

Bytes rawDeflate(const std::string& input) {
  z_stream stream{};
  const int initialized = deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  EXPECT_EQ(initialized, Z_OK);
  if (initialized != Z_OK) return {};
  Bytes output(deflateBound(&stream, input.size()));
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());
  EXPECT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  output.resize(stream.total_out);
  EXPECT_EQ(deflateEnd(&stream), Z_OK);
  return output;
}

struct Entry {
  std::string name;
  std::string contents;
  uint16_t method = 0;
  Bytes extra = {0x34, 0x12, 0x04, 0x00, 0x50, 0x4B, 0x01, 0x02};
  std::string comment = "entry comment PK\x01\x02";
};

struct Archive {
  std::vector<Entry> entries;
  std::vector<Bytes> encoded;
  std::vector<uint32_t> crc;
  std::vector<uint32_t> localOffsets;
  std::vector<uint32_t> directoryOffsets;
  Bytes bytes;
  uint32_t centralOffset = 0;
  uint32_t endOffset = 0;

  explicit Archive(std::vector<Entry> input, const std::string& archiveComment = "archive comment",
                   size_t prefix = 0x12345)
      : entries(std::move(input)), bytes(prefix, 0xA5) {
    for (const auto& entry : entries) {
      encoded.push_back(entry.method == 8 ? rawDeflate(entry.contents)
                                          : Bytes(entry.contents.begin(), entry.contents.end()));
      crc.push_back(static_cast<uint32_t>(
          crc32(0, reinterpret_cast<const Bytef*>(entry.contents.data()), entry.contents.size())));
      localOffsets.push_back(static_cast<uint32_t>(bytes.size()));
      Bytes header(30);
      put32(header, 0, 0x04034B50);
      put16(header, 4, 20);
      put16(header, 6, 0x0800);
      put16(header, 8, entry.method);
      put16(header, 10, 0x9ABC);
      put16(header, 12, 0x5678);
      put32(header, 14, crc.back());
      put32(header, 18, static_cast<uint32_t>(encoded.back().size()));
      put32(header, 22, static_cast<uint32_t>(entry.contents.size()));
      put16(header, 26, static_cast<uint16_t>(entry.name.size()));
      put16(header, 28, static_cast<uint16_t>(entry.extra.size()));
      append(bytes, header);
      append(bytes, entry.name);
      append(bytes, entry.extra);
      append(bytes, encoded.back());
    }

    centralOffset = static_cast<uint32_t>(bytes.size());
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto& entry = entries[i];
      directoryOffsets.push_back(static_cast<uint32_t>(bytes.size()));
      Bytes header(46);
      put32(header, 0, 0x02014B50);
      put16(header, 4, 0x031E);
      put16(header, 6, 20);
      put16(header, 8, 0x0800);
      put16(header, 10, entry.method);
      put16(header, 12, 0x9ABC);
      put16(header, 14, 0x5678);
      put32(header, 16, crc[i]);
      put32(header, 20, static_cast<uint32_t>(encoded[i].size()));
      put32(header, 24, static_cast<uint32_t>(entry.contents.size()));
      put16(header, 28, static_cast<uint16_t>(entry.name.size()));
      put16(header, 30, static_cast<uint16_t>(entry.extra.size()));
      put16(header, 32, static_cast<uint16_t>(entry.comment.size()));
      put16(header, 36, 0x1234);
      put32(header, 38, 0x81A40020);
      put32(header, 42, localOffsets[i]);
      append(bytes, header);
      append(bytes, entry.name);
      append(bytes, entry.extra);
      append(bytes, entry.comment);
    }

    endOffset = static_cast<uint32_t>(bytes.size());
    Bytes end(22);
    put32(end, 0, 0x06054B50);
    put16(end, 8, static_cast<uint16_t>(entries.size()));
    put16(end, 10, static_cast<uint16_t>(entries.size()));
    put32(end, 12, endOffset - centralOffset);
    put32(end, 16, centralOffset);
    put16(end, 20, static_cast<uint16_t>(archiveComment.size()));
    append(bytes, end);
    append(bytes, archiveComment);
  }
};

Archive ordinaryArchive() {
  return Archive({{"first.bin", "first payload"},
                  {"middle.bin", "middle payload is longer"},
                  {"last.bin", "last payload has a different size again"}});
}

Bytes tailDirectory(const Bytes& entry) {
  Bytes bytes(128, 0xA5);
  Bytes end(22);
  put32(end, 0, 0x06054B50);
  put16(end, 8, 1);
  put16(end, 10, 1);
  put32(end, 12, static_cast<uint32_t>(entry.size()));
  put32(end, 16, 150);
  append(bytes, end);
  append(bytes, entry);
  return bytes;
}

template <typename Target>
void sortTargets(std::vector<Target>& targets) {
  std::sort(targets.begin(), targets.end(),
            [](const Target& a, const Target& b) { return a.hash < b.hash || (a.hash == b.hash && a.len < b.len); });
}

std::vector<ZipFile::SizeTarget> sizeTargets(const Archive& archive) {
  std::vector<ZipFile::SizeTarget> targets;
  for (size_t i = 0; i < archive.entries.size(); ++i) {
    const auto& name = archive.entries[i].name;
    targets.push_back(
        {ZipFile::fnvHash64(name.data(), name.size()), static_cast<uint16_t>(name.size()), static_cast<uint16_t>(i)});
  }
  sortTargets(targets);
  return targets;
}

std::vector<ZipFile::EntryTarget> identityTargets(const Archive& archive) {
  std::vector<ZipFile::EntryTarget> targets;
  for (size_t i = 0; i < archive.entries.size(); ++i) {
    const auto& name = archive.entries[i].name;
    targets.push_back({ZipFile::fnvHash64(name.data(), name.size()), static_cast<uint16_t>(name.size()),
                       static_cast<uint16_t>(i), name.c_str()});
  }
  sortTargets(targets);
  return targets;
}

void expectSize(ZipFile& zip, const Entry& entry) {
  size_t size = kUntouched;
  ASSERT_TRUE(zip.getInflatedFileSize(entry.name.c_str(), &size));
  EXPECT_EQ(size, entry.contents.size());
}

std::vector<std::string> enumerate(ZipFile& zip) {
  std::vector<std::string> paths;
  EXPECT_TRUE(zip.enumerateFilePaths([&](std::string_view path) { paths.emplace_back(path); }));
  return paths;
}

struct Output : Print {
  Bytes bytes;
  size_t write(uint8_t byte) override {
    bytes.push_back(byte);
    return 1;
  }
  size_t write(const uint8_t* data, const size_t length) override {
    bytes.insert(bytes.end(), data, data + length);
    return length;
  }
};

class ZipFileLookupTest : public testing::Test {
 protected:
  const std::string path = "fixture.epub";
  void SetUp() override { Storage.reset(); }
  void install(const Archive& archive) { Storage.put(path, archive.bytes); }
};

TEST_F(ZipFileLookupTest, FirstMiddleLastMissingRepeatedAndWrappedQueries) {
  const auto archive = ordinaryArchive();
  install(archive);
  ZipFile zip(path);
  ASSERT_TRUE(zip.open());
  for (const size_t index : {0, 1, 2, 1, 1, 0}) {
    SCOPED_TRACE(index);
    expectSize(zip, archive.entries[index]);
    EXPECT_TRUE(zip.isOpen());
  }
  size_t missing = kUntouched;
  EXPECT_FALSE(zip.getInflatedFileSize("missing.bin", &missing));
  EXPECT_EQ(missing, kUntouched);
  expectSize(zip, archive.entries[2]);
  expectSize(zip, archive.entries[0]);
  ASSERT_TRUE(zip.close());
  EXPECT_FALSE(zip.isOpen());
}

TEST_F(ZipFileLookupTest, AutomaticOpenCloseAndExplicitReopenPreserveResults) {
  const auto archive = ordinaryArchive();
  install(archive);
  ZipFile zip(path);
  for (const auto& entry : archive.entries) {
    expectSize(zip, entry);
    EXPECT_FALSE(zip.isOpen());
  }
  ASSERT_TRUE(zip.open());
  expectSize(zip, archive.entries.back());
  ASSERT_TRUE(zip.close());
  ASSERT_TRUE(zip.open());
  expectSize(zip, archive.entries.front());
  EXPECT_EQ(enumerate(zip), (std::vector<std::string>{"first.bin", "middle.bin", "last.bin"}));
  EXPECT_TRUE(zip.isOpen());
  zip.close();
}

TEST_F(ZipFileLookupTest, NameBoundariesExtraFieldsAndCommentsKeepFollowingEntriesAligned) {
  const Archive archive({{std::string(255, 'a'), "accepted"},
                         {std::string(256, 'b'), "skipped"},
                         {std::string(65535, 'c'), "also skipped"},
                         {"after.bin", "following entry"}});
  install(archive);
  ZipFile zip(path);
  ASSERT_TRUE(zip.open());
  expectSize(zip, archive.entries[0]);
  for (const size_t index : {1, 2}) {
    size_t size = kUntouched;
    EXPECT_FALSE(zip.getInflatedFileSize(archive.entries[index].name.c_str(), &size));
    EXPECT_EQ(size, kUntouched);
  }
  expectSize(zip, archive.entries[3]);
  expectSize(zip, archive.entries[0]);
  const std::vector<std::string> expected = {archive.entries[0].name, "after.bin"};
  EXPECT_EQ(enumerate(zip), expected);
  auto targets = sizeTargets(archive);
  std::array<uint32_t, 4> sizes;
  sizes.fill(kUntouched);
  EXPECT_EQ(zip.fillUncompressedSizes(targets.data(), targets.size(), sizes.data(), sizes.size()), 2);
  EXPECT_EQ(sizes[0], archive.entries[0].contents.size());
  EXPECT_EQ(sizes[1], kUntouched);
  EXPECT_EQ(sizes[2], kUntouched);
  EXPECT_EQ(sizes[3], archive.entries[3].contents.size());
  auto entryTargets = identityTargets(archive);
  std::array<ZipFile::EntryIdentity, 4> identities{};
  EXPECT_EQ(zip.fillEntryIdentities(entryTargets.data(), entryTargets.size(), identities.data(), identities.size()), 2);
  EXPECT_TRUE(identities[0].found);
  EXPECT_FALSE(identities[1].found);
  EXPECT_FALSE(identities[2].found);
  EXPECT_TRUE(identities[3].found);
  ASSERT_TRUE(zip.loadAllFileStatSlims());
  auto cached = enumerate(zip);
  auto sortedExpected = expected;
  std::sort(cached.begin(), cached.end());
  std::sort(sortedExpected.begin(), sortedExpected.end());
  EXPECT_EQ(cached, sortedExpected);
  expectSize(zip, archive.entries[0]);
  expectSize(zip, archive.entries[3]);
  size_t size = kUntouched;
  EXPECT_FALSE(zip.getInflatedFileSize(archive.entries[1].name.c_str(), &size));
  EXPECT_EQ(size, kUntouched);
  zip.close();
}

TEST_F(ZipFileLookupTest, BatchSizesKeepDuplicatesMissingNamesAndOutputBounds) {
  const auto archive = ordinaryArchive();
  install(archive);
  auto targets = sizeTargets(archive);
  const auto& first = archive.entries[0].name;
  targets.push_back({ZipFile::fnvHash64(first.data(), first.size()), static_cast<uint16_t>(first.size()), 3});
  targets.push_back({ZipFile::fnvHash64(first.data(), first.size()), static_cast<uint16_t>(first.size()), 99});
  targets.push_back({ZipFile::fnvHash64("absent", 6), 6, 4});
  sortTargets(targets);
  std::array<uint32_t, 5> sizes;
  sizes.fill(kUntouched);
  ZipFile zip(path);
  ASSERT_TRUE(zip.open());
  EXPECT_EQ(zip.fillUncompressedSizes(targets.data(), targets.size(), sizes.data(), sizes.size()), 4);
  for (size_t i = 0; i < archive.entries.size(); ++i) EXPECT_EQ(sizes[i], archive.entries[i].contents.size());
  EXPECT_EQ(sizes[3], archive.entries[0].contents.size());
  EXPECT_EQ(sizes[4], kUntouched);
  expectSize(zip, archive.entries[1]);
  EXPECT_EQ(zip.fillUncompressedSizes(nullptr, targets.size(), sizes.data(), sizes.size()), 0);
  EXPECT_EQ(zip.fillUncompressedSizes(targets.data(), targets.size(), nullptr, sizes.size()), 0);
  EXPECT_EQ(zip.fillUncompressedSizes(targets.data(), 0, sizes.data(), sizes.size()), 0);
  zip.close();
}

TEST_F(ZipFileLookupTest, BatchIdentitiesRetainFullPathChecksAndOutputBounds) {
  const auto archive = ordinaryArchive();
  install(archive);
  auto targets = identityTargets(archive);
  const auto& first = archive.entries[0].name;
  const auto hash = ZipFile::fnvHash64(first.data(), first.size());
  const auto length = static_cast<uint16_t>(first.size());
  const std::string collision(first.size(), 'x');
  const std::string extended = first + "x";
  targets.push_back({hash, length, 3, first.c_str()});
  targets.push_back({hash, length, 4, collision.c_str()});
  targets.push_back({hash, length, 5, extended.c_str()});
  targets.push_back({hash, length, 6, nullptr});
  targets.push_back({hash, length, 99, first.c_str()});
  targets.push_back({ZipFile::fnvHash64("absent", 6), 6, 7, "absent"});
  sortTargets(targets);
  std::array<ZipFile::EntryIdentity, 8> identities;
  for (auto& identity : identities) identity = {kUntouched, kUntouched, kUntouched, false};
  ZipFile zip(path);
  EXPECT_EQ(zip.fillEntryIdentities(targets.data(), targets.size(), identities.data(), identities.size()), 4);
  for (size_t i = 0; i < archive.entries.size(); ++i) {
    EXPECT_TRUE(identities[i].found);
    EXPECT_EQ(identities[i].crc32, archive.crc[i]);
    EXPECT_EQ(identities[i].compressedSize, archive.encoded[i].size());
    EXPECT_EQ(identities[i].uncompressedSize, archive.entries[i].contents.size());
  }
  EXPECT_TRUE(identities[3].found);
  for (size_t i = 4; i < identities.size(); ++i) {
    EXPECT_FALSE(identities[i].found);
    EXPECT_EQ(identities[i].crc32, kUntouched);
    EXPECT_EQ(identities[i].compressedSize, kUntouched);
    EXPECT_EQ(identities[i].uncompressedSize, kUntouched);
  }
  EXPECT_EQ(zip.fillEntryIdentities(nullptr, targets.size(), identities.data(), identities.size()), 0);
  EXPECT_EQ(zip.fillEntryIdentities(targets.data(), targets.size(), nullptr, identities.size()), 0);
  EXPECT_EQ(zip.fillEntryIdentities(targets.data(), 0, identities.data(), identities.size()), 0);
}

TEST_F(ZipFileLookupTest, MetadataDecodesAllLittleEndianSizeAndCrcBytes) {
  auto archive = ordinaryArchive();
  const size_t offset = archive.directoryOffsets[1];
  put16(archive.bytes, offset + 10, 0x0108);
  put32(archive.bytes, offset + 16, 0xD3C2B1A0);
  put32(archive.bytes, offset + 20, 0x12345678);
  put32(archive.bytes, offset + 24, 0x89ABCDEF);
  put32(archive.bytes, offset + 42, 0x01234567);
  install(archive);
  ZipFile zip(path);
  size_t size = kUntouched;
  ASSERT_TRUE(zip.getInflatedFileSize("middle.bin", &size));
  EXPECT_EQ(size, 0x89ABCDEFu);
  auto sizes = sizeTargets(archive);
  std::array<uint32_t, 3> outputs{};
  EXPECT_EQ(zip.fillUncompressedSizes(sizes.data(), sizes.size(), outputs.data(), outputs.size()), 3);
  EXPECT_EQ(outputs[1], 0x89ABCDEFu);
  auto targets = identityTargets(archive);
  std::array<ZipFile::EntryIdentity, 3> identities{};
  EXPECT_EQ(zip.fillEntryIdentities(targets.data(), targets.size(), identities.data(), identities.size()), 3);
  EXPECT_TRUE(identities[1].found);
  EXPECT_EQ(identities[1].crc32, 0xD3C2B1A0u);
  EXPECT_EQ(identities[1].compressedSize, 0x12345678u);
  EXPECT_EQ(identities[1].uncompressedSize, 0x89ABCDEFu);
}

TEST_F(ZipFileLookupTest, StoredAndRealDeflatedReadsPreserveBytesMethodsSizesAndOffsets) {
  std::string contents(8193, '\0');
  for (size_t i = 0; i < contents.size(); ++i)
    contents[i] = static_cast<char>((i * 73 + (i >> 3) * 11 + (i >> 8)) & 255);
  const Archive archive({{"stored.bin", contents}, {"deflated.bin", contents + contents, 8}});
  ASSERT_GT(archive.localOffsets[0], 0xFFFFu);
  ASSERT_GT(archive.encoded[1].size(), 255u);
  install(archive);
  ZipFile zip(path);
  for (const auto& entry : archive.entries) {
    size_t size = kUntouched;
    std::unique_ptr<uint8_t, decltype(&std::free)> bytes(zip.readFileToMemory(entry.name.c_str(), &size, true),
                                                         &std::free);
    ASSERT_NE(bytes, nullptr);
    EXPECT_EQ(size, entry.contents.size());
    EXPECT_EQ(std::string(reinterpret_cast<char*>(bytes.get()), size), entry.contents);
    EXPECT_EQ(bytes.get()[size], 0);
    Output out;
    ASSERT_TRUE(zip.readFileToStream(entry.name.c_str(), out, 73));
    EXPECT_EQ(std::string(out.bytes.begin(), out.bytes.end()), entry.contents);
  }
  Output stored;
  ASSERT_TRUE(zip.readStoredFileToStream("stored.bin", stored));
  EXPECT_EQ(std::string(stored.bytes.begin(), stored.bytes.end()), contents);
  Output deflated;
  EXPECT_FALSE(zip.readStoredFileToStream("deflated.bin", deflated));
  EXPECT_TRUE(deflated.bytes.empty());
}

TEST_F(ZipFileLookupTest, CompressionMethodHighByteIsNotTruncated) {
  for (const uint16_t method : {0x0100, 0x0108}) {
    SCOPED_TRACE(method);
    const Archive archive({{"unsupported.bin", "nonempty data", method}});
    install(archive);
    ZipFile zip(path);
    size_t size = kUntouched;
    EXPECT_EQ(zip.readFileToMemory("unsupported.bin", &size), nullptr);
    EXPECT_EQ(size, kUntouched);
    Output out;
    EXPECT_FALSE(zip.readFileToStream("unsupported.bin", out, 16));
    EXPECT_TRUE(out.bytes.empty());
  }
}

TEST_F(ZipFileLookupTest, ShortOrFailedFixedHeadersNeverPublishAndRecoverOnTheSameOpenFile) {
  const auto archive = ordinaryArchive();
  install(archive);
  for (const int result : {-1, 0, 1, 3, 4, 5, 10, 24, 28, 42, 45}) {
    SCOPED_TRACE(result);
    ZipFile zip(path);
    ASSERT_TRUE(zip.open());
    expectSize(zip, archive.entries[0]);
    Storage.failReadOnce(path, archive.directoryOffsets[1], result);
    size_t size = kUntouched;
    EXPECT_FALSE(zip.getInflatedFileSize("middle.bin", &size));
    EXPECT_EQ(size, kUntouched);
    EXPECT_TRUE(zip.isOpen());
    expectSize(zip, archive.entries[2]);
    expectSize(zip, archive.entries[1]);
    zip.close();
  }
}

TEST_F(ZipFileLookupTest, FailedFullMapPopulationClearsPartialEntriesAndRecovers) {
  const auto archive = ordinaryArchive();
  install(archive);
  ZipFile zip(path);
  ASSERT_TRUE(zip.open());
  Storage.failReadOnce(path, archive.directoryOffsets[1], 45);
  EXPECT_FALSE(zip.loadAllFileStatSlims());
  expectSize(zip, archive.entries[1]);
  expectSize(zip, archive.entries[2]);
  ASSERT_TRUE(zip.loadAllFileStatSlims());
  for (const auto& entry : archive.entries) expectSize(zip, entry);
  zip.close();
}

TEST_F(ZipFileLookupTest, EnumerationReportsShortHeadersAndRecoversWithoutRetainingPartialResults) {
  const auto archive = ordinaryArchive();
  install(archive);
  ZipFile zip(path);
  ASSERT_TRUE(zip.open());
  Storage.failReadOnce(path, archive.directoryOffsets[1], 45);
  std::vector<std::string> paths;
  EXPECT_FALSE(zip.enumerateFilePaths([&](std::string_view name) { paths.emplace_back(name); }));
  EXPECT_EQ(paths, (std::vector<std::string>{"first.bin"}));
  EXPECT_EQ(enumerate(zip), (std::vector<std::string>{"first.bin", "middle.bin", "last.bin"}));
  zip.close();
}

TEST_F(ZipFileLookupTest, BatchLookupsPublishOnlyCompleteEntriesAndRecover) {
  const auto archive = ordinaryArchive();
  install(archive);
  auto sizes = sizeTargets(archive);
  auto targets = identityTargets(archive);
  ZipFile zip(path);
  ASSERT_TRUE(zip.open());
  std::array<uint32_t, 3> output;
  output.fill(kUntouched);
  Storage.failReadOnce(path, archive.directoryOffsets[1], 45);
  EXPECT_EQ(zip.fillUncompressedSizes(sizes.data(), sizes.size(), output.data(), output.size()), 1);
  EXPECT_EQ(output[0], archive.entries[0].contents.size());
  EXPECT_EQ(output[1], kUntouched);
  EXPECT_EQ(output[2], kUntouched);
  EXPECT_EQ(zip.fillUncompressedSizes(sizes.data(), sizes.size(), output.data(), output.size()), 3);
  std::array<ZipFile::EntryIdentity, 3> identities{};
  Storage.failReadOnce(path, archive.directoryOffsets[1], 45);
  EXPECT_EQ(zip.fillEntryIdentities(targets.data(), targets.size(), identities.data(), identities.size()), 1);
  EXPECT_TRUE(identities[0].found);
  EXPECT_FALSE(identities[1].found);
  EXPECT_FALSE(identities[2].found);
  EXPECT_EQ(zip.fillEntryIdentities(targets.data(), targets.size(), identities.data(), identities.size()), 3);
  EXPECT_TRUE(identities[1].found);
  EXPECT_TRUE(identities[2].found);
  expectSize(zip, archive.entries[0]);
  zip.close();
}

TEST_F(ZipFileLookupTest, NameReadAndEntrySkipFailuresDoNotAdvanceTheCursor) {
  const auto archive = ordinaryArchive();
  install(archive);
  ZipFile zip(path);
  ASSERT_TRUE(zip.open());
  expectSize(zip, archive.entries[0]);
  Storage.failReadOnce(path, archive.directoryOffsets[1] + 46, 3);
  size_t size = kUntouched;
  EXPECT_FALSE(zip.getInflatedFileSize("middle.bin", &size));
  EXPECT_EQ(size, kUntouched);
  expectSize(zip, archive.entries[1]);
  Storage.failSeekOnce(path, archive.directoryOffsets[2]);
  size = kUntouched;
  EXPECT_FALSE(zip.getInflatedFileSize("middle.bin", &size));
  EXPECT_EQ(size, kUntouched);
  expectSize(zip, archive.entries[1]);
  Storage.failSeekOnce(path, archive.centralOffset);
  EXPECT_FALSE(zip.loadAllFileStatSlims());
  expectSize(zip, archive.entries[0]);
  zip.close();
}

TEST_F(ZipFileLookupTest, MalformedSignaturesKeepTheExistingDirectoryBoundaryAndRecover) {
  auto archive = ordinaryArchive();
  const size_t offset = archive.directoryOffsets[1];
  put32(archive.bytes, offset, 0x02014B51);
  install(archive);
  ZipFile zip(path);
  ASSERT_TRUE(zip.open());
  expectSize(zip, archive.entries[0]);
  size_t size = kUntouched;
  EXPECT_FALSE(zip.getInflatedFileSize("middle.bin", &size));
  EXPECT_EQ(size, kUntouched);
  EXPECT_FALSE(zip.getInflatedFileSize("last.bin", &size));
  EXPECT_EQ(size, kUntouched);
  EXPECT_EQ(enumerate(zip), (std::vector<std::string>{"first.bin"}));
  Storage.overwrite(path, offset, {0x50, 0x4B, 0x01, 0x02});
  expectSize(zip, archive.entries[1]);
  expectSize(zip, archive.entries[2]);
  zip.close();
}

TEST_F(ZipFileLookupTest, PhysicallyTruncatedFixedHeadersFailWithoutReadingUninitializedFields) {
  const auto archive = ordinaryArchive();
  const auto start = archive.bytes.begin() + archive.directoryOffsets[0];
  for (const size_t length : {0, 1, 3, 4, 10, 28, 42, 45}) {
    SCOPED_TRACE(length);
    Storage.put(path, tailDirectory(Bytes(start, start + length)));
    ZipFile zip(path);
    size_t size = kUntouched;
    EXPECT_FALSE(zip.getInflatedFileSize("first.bin", &size));
    EXPECT_EQ(size, kUntouched);
  }
}

TEST_F(ZipFileLookupTest, TruncatedNamesAndUnreachableExtraCommentFieldsFailSafely) {
  const auto archive = ordinaryArchive();
  const auto start = archive.bytes.begin() + archive.directoryOffsets[0];
  Bytes header(start, start + 46);
  for (const auto lengths : {std::array<uint16_t, 3>{10, 0, 0}, {256, 0, 0}, {2, 100, 0}, {2, 0, 100}}) {
    SCOPED_TRACE(lengths[0]);
    put16(header, 28, lengths[0]);
    put16(header, 30, lengths[1]);
    put16(header, 32, lengths[2]);
    Bytes entry = header;
    append(entry, std::string("xy"));
    Storage.put(path, tailDirectory(entry));
    ZipFile zip(path);
    size_t size = kUntouched;
    EXPECT_FALSE(zip.getInflatedFileSize("xy", &size));
    EXPECT_EQ(size, kUntouched);
    EXPECT_FALSE(zip.loadAllFileStatSlims());
    std::vector<std::string> paths;
    EXPECT_FALSE(zip.enumerateFilePaths([&](std::string_view name) { paths.emplace_back(name); }));
    EXPECT_TRUE(paths.empty());
  }
}

TEST_F(ZipFileLookupTest, ArchiveCommentRetainsTheExistingOneKilobyteEocdScanBoundary) {
  const Archive accepted({{"first.bin", "content"}}, std::string(1002, 'c'));
  install(accepted);
  ZipFile zip(path);
  expectSize(zip, accepted.entries[0]);
  const Archive outsideRange({{"first.bin", "content"}}, std::string(1003, 'c'));
  install(outsideRange);
  ZipFile other(path);
  size_t size = kUntouched;
  EXPECT_FALSE(other.getInflatedFileSize("first.bin", &size));
  EXPECT_EQ(size, kUntouched);
}

TEST_F(ZipFileLookupTest, EmptyAndInvalidArchivesDoNotProduceEntries) {
  const Archive empty({}, "", 0);
  install(empty);
  ZipFile zip(path);
  size_t size = kUntouched;
  EXPECT_FALSE(zip.getInflatedFileSize("missing.bin", &size));
  EXPECT_EQ(size, kUntouched);
  EXPECT_TRUE(enumerate(zip).empty());
  EXPECT_TRUE(zip.loadAllFileStatSlims());
  for (const size_t length : {0, 21, 22, 80}) {
    Storage.put(path, Bytes(length, 0xA5));
    ZipFile invalid(path);
    EXPECT_FALSE(invalid.getInflatedFileSize("missing.bin", &size));
    EXPECT_EQ(size, kUntouched);
    EXPECT_FALSE(invalid.loadAllFileStatSlims());
    EXPECT_FALSE(invalid.enumerateFilePaths([](std::string_view) {}));
  }
}
}  // namespace
