#pragma once
#include <HalStorage.h>
#include <InflateStream.h>
#include <Memory.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

enum class ZipStreamStatus {
  More,
  Done,
  Error,
};

struct ZipStreamInflateCtx {
  InflateStream reader;
  HalFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

class ZipFileStreamReader {
 public:
  ZipFileStreamReader() = default;
  ~ZipFileStreamReader();
  ZipFileStreamReader(const ZipFileStreamReader&) = delete;
  ZipFileStreamReader& operator=(const ZipFileStreamReader&) = delete;

  bool begin(const std::string& zipPath, const char* filename, size_t chunkSize);
  ZipStreamStatus pump(Print& out, size_t maxOutputBytes = 0);
  void abort();
  size_t produced() const { return totalProduced; }

 private:
  std::string zipPath;
  uint16_t method = 0;
  uint32_t dataOffset = 0;
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  size_t chunkSize = 0;
  size_t totalProduced = 0;
  size_t compressedConsumed = 0;
  uint8_t* readBuffer = nullptr;
  uint8_t* outputBuffer = nullptr;
  ZipStreamInflateCtx inflateCtx;
  bool active = false;
};

class ZipFile {
  friend class ZipFileStreamReader;

 public:
  struct FileStatSlim {
    uint16_t method;             // Compression method
    uint32_t compressedSize;     // Compressed size
    uint32_t uncompressedSize;   // Uncompressed size
    uint32_t localHeaderOffset;  // Offset of local file header
  };

  // One resolved entry, tied to its ZipFile instance. It owns neither a file
  // handle nor buffers; sizing closes the archive before parser allocation.
  class StreamEntry {
    friend class ZipFile;

   public:
    size_t inflatedSize() const { return stat.uncompressedSize; }

   private:
    const ZipFile* owner = nullptr;
    FileStatSlim stat = {};
    size_t archiveSize = 0;
    uint32_t centralEntryOffset = 0;
    uint32_t crc32 = 0;
    uint32_t archiveModified = 0;
  };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint16_t totalEntries;
    bool isSet;
  };

  // Target for batch uncompressed size lookup (sorted by hash, then len)
  struct SizeTarget {
    uint64_t hash;   // FNV-1a 64-bit hash of normalized path
    uint16_t len;    // Length of path for collision reduction
    uint16_t index;  // Caller's index (e.g. spine index)
  };

  // Target and result for batch central-directory identity lookup. The full
  // path check prevents a hash collision from treating unrelated files as
  // byte-identical.
  struct EntryTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
    const char* path;
  };

  struct EntryIdentity {
    uint32_t crc32 = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    bool found = false;
  };

  // FNV-1a 64-bit hash computed from char buffer (no std::string allocation)
  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  const std::string& filePath;
  HalFile file;
  ZipDetails zipDetails = {0, 0, false};
  std::unordered_map<std::string, FileStatSlim> fileStatSlimCache;

  // Cursor for sequential central-dir scanning optimization
  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  struct CentralDirectoryHeader {
    FileStatSlim stat;
    uint32_t crc32;
    uint16_t nameLength;
    uint16_t extraLength;
    uint16_t commentLength;
  };

  enum class CentralDirectoryReadStatus { Entry, End, Error };

  bool seekCentralDirectory();
  CentralDirectoryReadStatus readCentralDirectoryHeader(CentralDirectoryHeader& header);
  bool readCentralDirectoryName(const CentralDirectoryHeader& header, char (&name)[256]);
  bool findFileStatSlim(const char* filename, FileStatSlim* fileStat, StreamEntry* streamEntry = nullptr);
  bool wrapCentralDirectorySearch(uint32_t startPos, bool& wrapped);
  void captureStreamEntry(StreamEntry* entry, const CentralDirectoryHeader& header, uint32_t entryStart);
  static int matchUncompressedSizes(const CentralDirectoryHeader& header, const char* name, const SizeTarget* targets,
                                    size_t targetCount, uint32_t* sizes, size_t sizeCount);
  static int matchEntryIdentities(const CentralDirectoryHeader& header, const char* name, const EntryTarget* targets,
                                  size_t targetCount, EntryIdentity* identities, size_t identityCount);

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  long getDataOffset(const FileStatSlim& fileStat);
  bool loadZipDetails();
  bool matchesStreamArchive(const StreamEntry& entry);
  bool validateStreamEntry(const char* filename, const StreamEntry& entry);
  bool streamFileStat(const char* filename, const FileStatSlim& stat, Print& out, size_t chunkSize, bool allowEarlyStop,
                      uint32_t started);
  bool streamStoredFile(const char* filename, const FileStatSlim& stat, Print& out, size_t chunkSize,
                        bool allowEarlyStop, uint32_t lookupUs);
  bool streamDeflatedFile(const char* filename, const FileStatSlim& stat, Print& out, size_t chunkSize,
                          bool allowEarlyStop, uint32_t lookupUs);

 public:
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  ~ZipFile() = default;
  // Zip file can be opened and closed by hand in order to allow for quick calculation of inflated file size
  // It is NOT recommended to pre-open it for any kind of inflation due to memory constraints
  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool loadAllFileStatSlims();
  bool getInflatedFileSize(const char* filename, size_t* size);
  // Resolve once for size-then-stream consumers without pre-opening for
  // inflation. Streaming reopens and checks the exact central entry, including
  // its full name and identity, rather than trusting a caller-provided offset.
  bool resolveStreamEntry(const char* filename, StreamEntry& entry);
  // Batch lookup: scan ZIP central dir once and fill sizes for matching targets.
  // targets must be sorted by (hash, len). sizes[target.index] receives uncompressedSize.
  // Returns number of targets matched.
  int fillUncompressedSizes(const SizeTarget* targets, size_t targetCount, uint32_t* sizes, size_t sizeCount);
  // Batch lookup for duplicate detection. Targets must be sorted by (hash,
  // len); identities[target.index] receives the ZIP entry's CRC and sizes.
  int fillEntryIdentities(const EntryTarget* targets, size_t targetCount, EntryIdentity* identities,
                          size_t identityCount);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  bool readStoredFileToStream(const char* filename, Print& out);
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize, bool allowEarlyStop = false);
  bool readFileToStream(const char* filename, const StreamEntry& entry, Print& out, size_t chunkSize,
                        bool allowEarlyStop = false);
  std::unique_ptr<ZipFileStreamReader> openFileStream(const char* filename, size_t chunkSize);

  template <typename F>
  bool enumerateFilePaths(F&& callback) {
    if (!fileStatSlimCache.empty()) {
      for (const auto& entry : fileStatSlimCache) {
        callback(std::string_view{entry.first});
      }
      return true;
    }

    const bool wasOpen = isOpen();
    if (!wasOpen && !open()) {
      return false;
    }

    const ScopedCleanup closeAfterEnumeration{[this, wasOpen] {
      if (!wasOpen) close();
    }};
    if (!seekCentralDirectory()) {
      return false;
    }

    char itemName[256];
    bool success = true;

    while (file.available()) {
      CentralDirectoryHeader header;
      const auto status = readCentralDirectoryHeader(header);
      if (status != CentralDirectoryReadStatus::Entry) {
        success = status == CentralDirectoryReadStatus::End;
        break;
      }
      if (!readCentralDirectoryName(header, itemName)) {
        success = false;
        break;
      }

      if (header.nameLength < sizeof(itemName)) {
        callback(std::string_view{itemName, header.nameLength});
      }
    }

    return success;
  }
};
