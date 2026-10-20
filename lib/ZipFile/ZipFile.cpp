#include "ZipFile.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

struct ZipInflateCtx {
  HalFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;
constexpr size_t ONE_SHOT_DEFLATE_MAX_COMPRESSED_BYTES = 32768;

uint16_t readLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t readLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

int findEndOfCentralDirectory(const uint8_t* bytes, const int length) {
  for (int offset = length - 22; offset >= 0; --offset) {
    if (readLe32(bytes + offset) == 0x06054b50) return offset;
  }
  return -1;
}

bool matchesEntryName(const uint16_t length, const char (&name)[256], const char* filename) {
  return length < sizeof(name) && strcmp(name, filename) == 0;
}

bool sameFileStat(const ZipFile::FileStatSlim& a, const ZipFile::FileStatSlim& b) {
  return a.method == b.method && a.compressedSize == b.compressedSize && a.uncompressedSize == b.uncompressedSize &&
         a.localHeaderOffset == b.localHeaderOffset;
}

bool writeStreamChunk(Print& out, const uint8_t* data, const size_t length, const bool allowEarlyStop,
                      uint32_t& writeUs) {
  if (length == 0) return true;
  const uint32_t started = micros();
  const size_t written = out.write(data, length);
  writeUs += micros() - started;
  if (written == length) return true;
  if (!allowEarlyStop) {
    LOG_ERR("ZIP", "Failed to write all output bytes to stream");
  }
  return false;
}

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

size_t zipFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<ZipInflateCtx*>(vctx);
  if (ctx->fileRemaining == 0) return 0;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const int result = ctx->file->read(ctx->readBuf, toRead);
  if (result < 0) {
    LOG_ERR("ZIP", "Failed to read compressed data: %d", result);
    return 0;
  }
  const size_t bytesRead = static_cast<size_t>(result);
  ctx->fileRemaining -= bytesRead;

  *data = ctx->readBuf;
  return bytesRead;
}

size_t zipStreamFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<ZipStreamInflateCtx*>(vctx);
  if (!ctx->file || ctx->fileRemaining == 0) return 0;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const int result = ctx->file->read(ctx->readBuf, toRead);
  if (result < 0) {
    LOG_ERR("ZIP", "Failed to read cooperative compressed data: %d", result);
    return 0;
  }
  const size_t bytesRead = static_cast<size_t>(result);
  ctx->fileRemaining -= bytesRead;

  *data = ctx->readBuf;
  return bytesRead;
}
}  // namespace

ZipFileStreamReader::~ZipFileStreamReader() { abort(); }

bool ZipFileStreamReader::begin(const std::string& zipPathIn, const char* filename, const size_t chunkSizeIn) {
  abort();
  if (!filename || filename[0] == '\0' || chunkSizeIn == 0) {
    return false;
  }

  ZipFile zip(zipPathIn);
  const ScopedOpenClose zipOpen{zip};
  if (!zipOpen) return false;

  ZipFile::FileStatSlim fileStat = {};
  if (!zip.loadFileStatSlim(filename, &fileStat)) return false;

  const long offset = zip.getDataOffset(fileStat);
  if (offset < 0) return false;

  zipPath = zipPathIn;
  method = fileStat.method;
  dataOffset = static_cast<uint32_t>(offset);
  compressedSize = fileStat.compressedSize;
  uncompressedSize = fileStat.uncompressedSize;
  chunkSize = chunkSizeIn;
  totalProduced = 0;
  compressedConsumed = 0;

  readBuffer = static_cast<uint8_t*>(malloc(chunkSize));
  if (!readBuffer) {
    LOG_ERR("ZIP", "Failed to allocate cooperative read buffer (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap(), chunkSize);
    abort();
    return false;
  }
  outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
  if (!outputBuffer) {
    LOG_ERR("ZIP", "Failed to allocate cooperative output buffer (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap(), chunkSize);
    abort();
    return false;
  }

  if (method == ZIP_METHOD_DEFLATED) {
    inflateCtx.fileRemaining = compressedSize;
    inflateCtx.readBuf = readBuffer;
    inflateCtx.readBufSize = chunkSize;
    if (!inflateCtx.reader.init(true)) {
      LOG_ERR("ZIP", "Failed to init cooperative inflate reader (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap(), chunkSize);
      abort();
      return false;
    }
    inflateCtx.reader.setFill(zipStreamFillCallback, &inflateCtx);
  } else if (method != ZIP_METHOD_STORED) {
    LOG_ERR("ZIP", "Unsupported compression method");
    abort();
    return false;
  }

  active = true;
  return true;
}

ZipStreamStatus ZipFileStreamReader::pump(Print& out, const size_t maxOutputBytes) {
  if (!active) return ZipStreamStatus::Error;

  HalFile zipFile;
  if (!Storage.openFileForRead("ZIP", zipPath, zipFile)) {
    return ZipStreamStatus::Error;
  }

  if (method == ZIP_METHOD_STORED) {
    if (!zipFile.seek(dataOffset + totalProduced)) {
      zipFile.close();
      return ZipStreamStatus::Error;
    }
    const size_t remaining = static_cast<size_t>(uncompressedSize) - totalProduced;
    const size_t budget = maxOutputBytes > 0 ? std::min(remaining, maxOutputBytes) : remaining;
    const size_t toRead = std::min(chunkSize, budget);
    if (toRead == 0) {
      zipFile.close();
      active = false;
      return ZipStreamStatus::Done;
    }
    const int readResult = zipFile.read(outputBuffer, toRead);
    zipFile.close();
    if (readResult <= 0) {
      LOG_ERR("ZIP", "Failed to read stored stream data: %d", readResult);
      return ZipStreamStatus::Error;
    }
    const size_t dataRead = static_cast<size_t>(readResult);
    if (out.write(outputBuffer, dataRead) != dataRead) return ZipStreamStatus::Error;
    totalProduced += dataRead;
    if (totalProduced == static_cast<size_t>(uncompressedSize)) {
      active = false;
      return ZipStreamStatus::Done;
    }
    return ZipStreamStatus::More;
  }

  if (!zipFile.seek(dataOffset + compressedConsumed)) {
    zipFile.close();
    return ZipStreamStatus::Error;
  }

  inflateCtx.file = &zipFile;
  size_t pumped = 0;
  bool success = false;
  ZipStreamStatus result = ZipStreamStatus::More;

  while (true) {
    size_t produced = 0;
    const size_t beforeRemaining = inflateCtx.fileRemaining;
    const size_t outputLimit =
        maxOutputBytes > 0 ? std::min(chunkSize, maxOutputBytes - pumped) : static_cast<size_t>(chunkSize);
    if (outputLimit == 0) {
      success = true;
      break;
    }

    const InflateStream::Status status = inflateCtx.reader.readAtMost(outputBuffer, outputLimit, &produced);
    compressedConsumed += beforeRemaining - inflateCtx.fileRemaining;
    inflateCtx.file = nullptr;

    totalProduced += produced;
    pumped += produced;
    if (totalProduced > static_cast<size_t>(uncompressedSize)) {
      LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
              static_cast<size_t>(uncompressedSize));
      result = ZipStreamStatus::Error;
      break;
    }

    if (produced > 0 && out.write(outputBuffer, produced) != produced) {
      result = ZipStreamStatus::Error;
      break;
    }

    if (status == InflateStream::Status::Done) {
      if (totalProduced != static_cast<size_t>(uncompressedSize)) {
        LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(uncompressedSize),
                totalProduced);
        result = ZipStreamStatus::Error;
      } else {
        active = false;
        result = ZipStreamStatus::Done;
      }
      success = result != ZipStreamStatus::Error;
      break;
    }

    if (status == InflateStream::Status::Error) {
      LOG_ERR("ZIP", "Decompression failed");
      result = ZipStreamStatus::Error;
      break;
    }

    if (maxOutputBytes > 0 && pumped >= maxOutputBytes) {
      success = true;
      result = ZipStreamStatus::More;
      break;
    }

    inflateCtx.file = &zipFile;
  }

  inflateCtx.file = nullptr;
  zipFile.close();
  if (!success && result == ZipStreamStatus::Error) {
    return ZipStreamStatus::Error;
  }
  return result;
}

void ZipFileStreamReader::abort() {
  active = false;
  inflateCtx.reader.deinit();
  inflateCtx.file = nullptr;
  inflateCtx.fileRemaining = 0;
  inflateCtx.readBuf = nullptr;
  inflateCtx.readBufSize = 0;
  if (readBuffer) {
    free(readBuffer);
    readBuffer = nullptr;
  }
  if (outputBuffer) {
    free(outputBuffer);
    outputBuffer = nullptr;
  }
  zipPath.clear();
  method = 0;
  dataOffset = 0;
  compressedSize = 0;
  uncompressedSize = 0;
  chunkSize = 0;
  totalProduced = 0;
  compressedConsumed = 0;
}

bool ZipFile::seekCentralDirectory() {
  if (!loadZipDetails()) return false;
  if (!file.seek(zipDetails.centralDirOffset)) {
    LOG_ERR("ZIP", "Failed to seek central directory");
    lastCentralDirPosValid = false;
    return false;
  }
  return true;
}

ZipFile::CentralDirectoryReadStatus ZipFile::readCentralDirectoryHeader(CentralDirectoryHeader& header) {
  uint8_t bytes[46];
  const int bytesRead = file.read(bytes, sizeof(bytes));
  if (bytesRead >= 4 && readLe32(bytes) != 0x02014b50) {
    return CentralDirectoryReadStatus::End;
  }
  if (bytesRead != static_cast<int>(sizeof(bytes))) {
    LOG_ERR("ZIP", "Failed to read central directory header: %d", bytesRead);
    lastCentralDirPosValid = false;
    return CentralDirectoryReadStatus::Error;
  }

  header.stat.method = readLe16(bytes + 10);
  header.crc32 = readLe32(bytes + 16);
  header.stat.compressedSize = readLe32(bytes + 20);
  header.stat.uncompressedSize = readLe32(bytes + 24);
  header.nameLength = readLe16(bytes + 28);
  header.extraLength = readLe16(bytes + 30);
  header.commentLength = readLe16(bytes + 32);
  header.stat.localHeaderOffset = readLe32(bytes + 42);
  return CentralDirectoryReadStatus::Entry;
}

bool ZipFile::readCentralDirectoryName(const CentralDirectoryHeader& header, char (&name)[256]) {
  bool nameRead;
  if (header.nameLength < sizeof(name)) {
    nameRead = file.read(name, header.nameLength) == header.nameLength;
    if (nameRead) {
      name[header.nameLength] = '\0';
    }
  } else {
    nameRead = file.seekCur(header.nameLength);
  }
  if (!nameRead || !file.seekCur(static_cast<uint32_t>(header.extraLength) + header.commentLength)) {
    LOG_ERR("ZIP", "Failed to read central directory name or skip entry fields");
    lastCentralDirPosValid = false;
    return false;
  }
  return true;
}

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!seekCentralDirectory()) return false;

  char itemName[256];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  while (file.available()) {
    CentralDirectoryHeader header;
    const auto status = readCentralDirectoryHeader(header);
    if (status == CentralDirectoryReadStatus::End) break;
    if (status == CentralDirectoryReadStatus::Error || !readCentralDirectoryName(header, itemName)) {
      fileStatSlimCache.clear();
      return false;
    }

    if (header.nameLength < sizeof(itemName)) {
      fileStatSlimCache.emplace(itemName, header.stat);
    }
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  return findFileStatSlim(filename, fileStat);
}

bool ZipFile::wrapCentralDirectorySearch(const uint32_t startPos, bool& wrapped) {
  if (wrapped || startPos == zipDetails.centralDirOffset) return false;
  if (!file.seek(zipDetails.centralDirOffset)) {
    LOG_ERR("ZIP", "Failed to wrap central directory cursor");
    lastCentralDirPosValid = false;
    return false;
  }
  wrapped = true;
  return true;
}

void ZipFile::captureStreamEntry(StreamEntry* entry, const CentralDirectoryHeader& header, const uint32_t entryStart) {
  if (!entry) return;
  entry->stat = header.stat;
  entry->archiveSize = file.size();
  entry->archiveModified = file.modificationTime();
  entry->centralEntryOffset = entryStart;
  entry->crc32 = header.crc32;
  entry->owner = this;
}

bool ZipFile::findFileStatSlim(const char* filename, FileStatSlim* fileStat, StreamEntry* streamEntry) {
  const uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  if (!file.seek(startPos)) {
    LOG_ERR("ZIP", "Failed to seek central directory cursor");
    lastCentralDirPosValid = false;
    return false;
  }

  char itemName[256];
  while (!wrapped || file.position() < startPos) {
    const uint32_t entryStart = file.position();
    CentralDirectoryHeader header;
    const auto status = readCentralDirectoryHeader(header);
    if (status == CentralDirectoryReadStatus::Error) return false;
    if (status == CentralDirectoryReadStatus::End) {
      if (!wrapCentralDirectorySearch(startPos, wrapped)) return false;
      continue;
    }

    if (!readCentralDirectoryName(header, itemName)) return false;

    if (matchesEntryName(header.nameLength, itemName, filename)) {
      *fileStat = header.stat;
      lastCentralDirPos = file.position();
      lastCentralDirPosValid = true;
      captureStreamEntry(streamEntry, header, entryStart);
      return true;
    }
  }

  return false;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  if (!file.seek(fileOffset)) {
    LOG_ERR("ZIP", "Failed to seek local file header");
    return -1;
  }
  const int readResult = file.read(pLocalHeader, localHeaderSize);

  if (readResult != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header: %d", readResult);
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = static_cast<uint8_t*>(malloc(scanRange));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return false;
  }

  if (!file.seek(fileSize - scanRange) || file.read(buffer, scanRange) != scanRange) {
    LOG_ERR("ZIP", "Failed to read EOCD scan buffer");
    free(buffer);
    return false;
  }

  const int foundOffset = findEndOfCentralDirectory(buffer, scanRange);

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    free(buffer);
    return false;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  memcpy(&zipDetails.totalEntries, buffer + foundOffset + 10, sizeof(zipDetails.totalEntries));
  memcpy(&zipDetails.centralDirOffset, buffer + foundOffset + 16, sizeof(zipDetails.centralDirOffset));
  zipDetails.isSet = true;

  free(buffer);
  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

bool ZipFile::resolveStreamEntry(const char* filename, StreamEntry& entry) {
  entry = {};
  if (!filename || filename[0] == '\0') return false;
  const ScopedOpenClose zip{*this};
  if (!zip || !loadZipDetails()) return false;
  return findFileStatSlim(filename, &entry.stat, &entry);
}

bool ZipFile::matchesStreamArchive(const StreamEntry& entry) {
  // Zero/unknown timestamps still require the exact central-entry identity;
  // they do not turn this handoff into a persistent or timestamp-only cache.
  return entry.owner == this && file.size() == entry.archiveSize && file.modificationTime() == entry.archiveModified;
}

bool ZipFile::validateStreamEntry(const char* filename, const StreamEntry& entry) {
  if (!filename || !matchesStreamArchive(entry)) return false;
  if (!file.seek(entry.centralEntryOffset)) return false;
  CentralDirectoryHeader header;
  if (readCentralDirectoryHeader(header) != CentralDirectoryReadStatus::Entry) return false;
  if (header.crc32 != entry.crc32 || !sameFileStat(header.stat, entry.stat)) {
    return false;
  }
  char name[256];
  return header.nameLength < sizeof(name) && readCentralDirectoryName(header, name) && strcmp(name, filename) == 0;
}

int ZipFile::matchUncompressedSizes(const CentralDirectoryHeader& header, const char* name, const SizeTarget* targets,
                                    const size_t targetCount, uint32_t* sizes, const size_t sizeCount) {
  const uint64_t hash = fnvHash64(name, header.nameLength);
  const SizeTarget key = {hash, header.nameLength, 0};
  const SizeTarget* const targetEnd = targets + targetCount;
  auto it = std::lower_bound(targets, targetEnd, key, [](const SizeTarget& a, const SizeTarget& b) {
    return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
  });

  int matched = 0;
  while (it != targetEnd && it->hash == hash && it->len == header.nameLength) {
    if (it->index < sizeCount) {
      sizes[it->index] = header.stat.uncompressedSize;
      ++matched;
    }
    ++it;
  }
  return matched;
}

int ZipFile::matchEntryIdentities(const CentralDirectoryHeader& header, const char* name, const EntryTarget* targets,
                                  const size_t targetCount, EntryIdentity* identities, const size_t identityCount) {
  const uint64_t hash = fnvHash64(name, header.nameLength);
  const EntryTarget key = {hash, header.nameLength, 0, nullptr};
  const EntryTarget* const targetEnd = targets + targetCount;
  auto it = std::lower_bound(targets, targetEnd, key, [](const EntryTarget& a, const EntryTarget& b) {
    return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
  });

  int matched = 0;
  while (it != targetEnd && it->hash == hash && it->len == header.nameLength) {
    if (it->index < identityCount && it->path != nullptr && memcmp(it->path, name, header.nameLength) == 0 &&
        it->path[header.nameLength] == '\0') {
      identities[it->index] = {header.crc32, header.stat.compressedSize, header.stat.uncompressedSize, true};
      ++matched;
    }
    ++it;
  }
  return matched;
}

int ZipFile::fillUncompressedSizes(const SizeTarget* targets, const size_t targetCount, uint32_t* sizes,
                                   const size_t sizeCount) {
  if (targets == nullptr || sizes == nullptr || targetCount == 0) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!seekCentralDirectory()) return 0;

  int matched = 0;
  const auto expectedMatches = static_cast<int>(targetCount);
  char itemName[256];

  while (file.available() && matched < expectedMatches) {
    CentralDirectoryHeader header;
    if (readCentralDirectoryHeader(header) != CentralDirectoryReadStatus::Entry) break;
    if (!readCentralDirectoryName(header, itemName)) break;
    if (header.nameLength >= sizeof(itemName)) continue;

    matched += matchUncompressedSizes(header, itemName, targets, targetCount, sizes, sizeCount);
  }

  return matched;
}

int ZipFile::fillEntryIdentities(const EntryTarget* targets, const size_t targetCount, EntryIdentity* identities,
                                 const size_t identityCount) {
  if (targets == nullptr || identities == nullptr || targetCount == 0) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!seekCentralDirectory()) return 0;

  int matched = 0;
  const auto expectedMatches = static_cast<int>(targetCount);
  char itemName[256];

  while (file.available() && matched < expectedMatches) {
    CentralDirectoryHeader header;
    if (readCentralDirectoryHeader(header) != CentralDirectoryReadStatus::Entry) break;
    if (!readCentralDirectoryName(header, itemName)) break;
    if (header.nameLength >= sizeof(itemName)) continue;

    matched += matchEntryIdentities(header, itemName, targets, targetCount, identities, identityCount);
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const int readResult = file.read(data, inflatedDataSize);

    if (readResult < 0 || static_cast<size_t>(readResult) != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read stored data: %d", readResult);
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    bool inflated = false;
    if (deflatedDataSize <= ONE_SHOT_DEFLATE_MAX_COMPRESSED_BYTES) {
      auto* compressedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
      if (compressedData) {
        const int readResult = file.read(compressedData, deflatedDataSize);
        if (readResult < 0 || static_cast<size_t>(readResult) != deflatedDataSize) {
          LOG_ERR("ZIP", "Failed to read compressed data: %d", readResult);
          free(compressedData);
          free(data);
          return nullptr;
        }

        InflateStream inflate;
        if (!inflate.init(false)) {
          LOG_ERR("ZIP", "Failed to init one-shot inflate stream");
          free(compressedData);
          free(data);
          return nullptr;
        }
        inflate.setSource(compressedData, deflatedDataSize);
        if (!inflate.read(data, inflatedDataSize)) {
          LOG_ERR("ZIP", "Failed to inflate file");
          free(compressedData);
          free(data);
          return nullptr;
        }
        free(compressedData);
        inflated = true;
      } else {
        LOG_DBG("ZIP", "Falling back to streaming inflate; compressed buffer alloc failed (%zu bytes)",
                static_cast<size_t>(deflatedDataSize));
      }
    }

    if (!inflated) {
      file.seek(fileOffset);
      auto* fileReadBuffer = static_cast<uint8_t*>(malloc(1024));
      if (!fileReadBuffer) {
        LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
        free(data);
        return nullptr;
      }

      ZipInflateCtx ctx;
      ctx.file = &file;
      ctx.fileRemaining = deflatedDataSize;
      ctx.readBuf = fileReadBuffer;
      ctx.readBufSize = 1024;

      // One-shot mode: `data` holds the entire output, so back-references
      // resolve inside it and no 32KB window is allocated.
      InflateStream inflate;
      if (!inflate.init(false)) {
        LOG_ERR("ZIP", "Failed to init inflate stream for %s", filename);
        free(fileReadBuffer);
        free(data);
        return nullptr;
      }
      inflate.setFill(zipFillCallback, &ctx);

      if (!inflate.read(data, inflatedDataSize)) {
        LOG_ERR("ZIP", "Failed to inflate file");
        free(fileReadBuffer);
        free(data);
        return nullptr;
      }
      free(fileReadBuffer);
    }

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readStoredFileToStream(const char* filename, Print& out) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;
  FileStatSlim stat = {};
  if (!loadFileStatSlim(filename, &stat)) return false;
  if (stat.method != ZIP_METHOD_STORED || stat.compressedSize != stat.uncompressedSize) {
    LOG_ERR("ZIP", "Optimizer entry must use STORE: %s", filename);
    return false;
  }
  const long offset = getDataOffset(stat);
  if (offset < 0 || !file.seek(offset)) return false;
  uint8_t buffer[256];
  size_t remaining = stat.uncompressedSize;
  while (remaining) {
    const size_t n = std::min(remaining, sizeof(buffer));
    if (file.read(buffer, n) != static_cast<int>(n) || out.write(buffer, n) != n) {
      LOG_ERR("ZIP", "Stored optimizer entry read/write failed: %s", filename);
      return false;
    }
    remaining -= n;
  }
  return true;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize, const bool allowEarlyStop) {
  const uint32_t started = micros();
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;
  return streamFileStat(filename, fileStat, out, chunkSize, allowEarlyStop, started);
}

bool ZipFile::readFileToStream(const char* filename, const StreamEntry& entry, Print& out, const size_t chunkSize,
                               const bool allowEarlyStop) {
  const uint32_t started = micros();
  if (entry.owner != this) return false;
  const ScopedOpenClose zip{*this};
  if (!zip || !validateStreamEntry(filename, entry)) return false;
  return streamFileStat(filename, entry.stat, out, chunkSize, allowEarlyStop, started);
}

bool ZipFile::streamFileStat(const char* filename, const FileStatSlim& stat, Print& out, const size_t chunkSize,
                             const bool allowEarlyStop, const uint32_t started) {
  const long fileOffset = getDataOffset(stat);
  if (fileOffset < 0 || !file.seek(fileOffset)) return false;
  const uint32_t lookupUs = micros() - started;
  if (stat.method == ZIP_METHOD_STORED) {
    return streamStoredFile(filename, stat, out, chunkSize, allowEarlyStop, lookupUs);
  }
  if (stat.method == ZIP_METHOD_DEFLATED) {
    return streamDeflatedFile(filename, stat, out, chunkSize, allowEarlyStop, lookupUs);
  }
  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}

bool ZipFile::streamStoredFile(const char* filename, const FileStatSlim& stat, Print& out, const size_t chunkSize,
                               const bool allowEarlyStop, const uint32_t lookupUs) {
  const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for buffer");
    return false;
  }

  uint32_t readUs = 0;
  uint32_t writeUs = 0;
  size_t remaining = stat.uncompressedSize;
  while (remaining > 0) {
    const uint32_t readStarted = micros();
    const int readResult = file.read(buffer, remaining < chunkSize ? remaining : chunkSize);
    readUs += micros() - readStarted;
    if (readResult <= 0) {
      LOG_ERR("ZIP", "Could not read more stored bytes: %d", readResult);
      free(buffer);
      return false;
    }
    const size_t dataRead = static_cast<size_t>(readResult);
    if (!writeStreamChunk(out, buffer, dataRead, allowEarlyStop, writeUs)) {
      free(buffer);
      return allowEarlyStop;
    }
    remaining -= dataRead;
  }

  free(buffer);
  LOG_DBG("ZIP", "Stream %s: lookup=%uus read=%uus write=%uus bytes=%u", filename, static_cast<unsigned>(lookupUs),
          static_cast<unsigned>(readUs), static_cast<unsigned>(writeUs), static_cast<unsigned>(stat.uncompressedSize));
  return true;
}

bool ZipFile::streamDeflatedFile(const char* filename, const FileStatSlim& stat, Print& out, const size_t chunkSize,
                                 const bool allowEarlyStop, const uint32_t lookupUs) {
  ZipInflateCtx ctx;
  ctx.file = &file;
  ctx.fileRemaining = stat.compressedSize;

  auto* fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
  if (!fileReadBuffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer (free=%u, maxAlloc=%u, chunk=%zu)",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap(), chunkSize);
    return false;
  }

  auto* outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
  if (!outputBuffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap(), chunkSize);
    free(fileReadBuffer);
    return false;
  }

  ctx.readBuf = fileReadBuffer;
  ctx.readBufSize = chunkSize;

  InflateStream inflate;
  if (!inflate.init(true)) {
    LOG_ERR("ZIP", "Failed to init inflate stream for %s (free=%u, maxAlloc=%u, chunk=%zu)", filename,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap(), chunkSize);
    free(outputBuffer);
    free(fileReadBuffer);
    return false;
  }
  inflate.setFill(zipFillCallback, &ctx);

  uint32_t readInflateUs = 0;
  uint32_t writeUs = 0;
  bool success = false;
  size_t totalProduced = 0;
  while (true) {
    size_t produced;
    const uint32_t inflateStarted = micros();
    const InflateStream::Status status = inflate.readAtMost(outputBuffer, chunkSize, &produced);
    readInflateUs += micros() - inflateStarted;

    totalProduced += produced;
    if (totalProduced > static_cast<size_t>(stat.uncompressedSize)) {
      LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
              static_cast<size_t>(stat.uncompressedSize));
      break;
    }
    if (!writeStreamChunk(out, outputBuffer, produced, allowEarlyStop, writeUs)) {
      success = allowEarlyStop;
      break;
    }
    if (status == InflateStream::Status::Done) {
      if (totalProduced != static_cast<size_t>(stat.uncompressedSize)) {
        LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(stat.uncompressedSize),
                totalProduced);
        break;
      }
      success = true;
      break;
    }
    if (status == InflateStream::Status::Error) {
      LOG_ERR("ZIP", "Decompression failed");
      break;
    }
  }

  free(outputBuffer);
  free(fileReadBuffer);
  LOG_DBG("ZIP", "Stream %s: ok=%d lookup=%uus read/inflate=%uus write=%uus bytes=%u", filename, success,
          static_cast<unsigned>(lookupUs), static_cast<unsigned>(readInflateUs), static_cast<unsigned>(writeUs),
          static_cast<unsigned>(totalProduced));
  return success;  // inflate destructor frees the decompressor state + window
}

std::unique_ptr<ZipFileStreamReader> ZipFile::openFileStream(const char* filename, const size_t chunkSize) {
  auto reader = makeUniqueNoThrow<ZipFileStreamReader>();
  if (!reader) {
    LOG_ERR("ZIP", "Failed to allocate cooperative stream reader");
    return nullptr;
  }
  if (!reader->begin(filePath, filename, chunkSize)) {
    return nullptr;
  }
  return reader;
}
