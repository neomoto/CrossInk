#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace txtlayout {
constexpr uint32_t PAGE_INDEX_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t PAGE_INDEX_VERSION = 5;
constexpr uint8_t LEGACY_PAGE_INDEX_VERSION = 4;
constexpr uint32_t MAX_INDEX_PAGES = 65535;
constexpr size_t PAGE_INDEX_HEADER_BYTES = 34;

// These are the existing on-disk fields, not a new cache format. Height affects
// TXT pagination through linesPerPage; the full viewport selects the disk slot.
struct PageIndexSpec {
  size_t sourceSize = 0;
  int32_t viewportWidth = 0;
  int32_t linesPerPage = 0;
  int32_t fontId = 0;
  int32_t verticalMargin = 0;
  int32_t horizontalMargin = 0;
  uint8_t paragraphAlignment = 0;

  bool operator==(const PageIndexSpec& other) const {
    return sourceSize == other.sourceSize && viewportWidth == other.viewportWidth &&
           linesPerPage == other.linesPerPage && fontId == other.fontId && verticalMargin == other.verticalMargin &&
           horizontalMargin == other.horizontalMargin && paragraphAlignment == other.paragraphAlignment;
  }
};

struct PageIndexHeader {
  PageIndexSpec spec;
  uint32_t pageCount = 0;
  uint8_t version = 0;
};

inline uint32_t readIndexUint32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

inline std::string pageIndexPath(const std::string& cachePath, const int width, const int height) {
  return cachePath + (width > height ? "/index.wide.bin" : "/index.bin");
}

inline bool readPageIndexHeader(HalFile& file, PageIndexHeader& header) {
  uint8_t bytes[PAGE_INDEX_HEADER_BYTES];
  if (file.read(bytes, sizeof(bytes)) != static_cast<int>(sizeof(bytes))) return false;
  if (readIndexUint32(bytes) != PAGE_INDEX_MAGIC) return false;
  header.version = bytes[4];
  header.spec = {readIndexUint32(bytes + 5),
                 static_cast<int32_t>(readIndexUint32(bytes + 9)),
                 static_cast<int32_t>(readIndexUint32(bytes + 13)),
                 static_cast<int32_t>(readIndexUint32(bytes + 17)),
                 static_cast<int32_t>(readIndexUint32(bytes + 21)),
                 static_cast<int32_t>(readIndexUint32(bytes + 25)),
                 bytes[29]};
  header.pageCount = readIndexUint32(bytes + 30);
  return true;
}

inline bool validPageIndexLength(const PageIndexHeader& header, const size_t size) {
  return header.pageCount > 0 && header.pageCount <= MAX_INDEX_PAGES &&
         size == PAGE_INDEX_HEADER_BYTES + header.pageCount * sizeof(uint32_t);
}

inline void recoverPageIndexBackup(const std::string& path) {
  const std::string backup = path + ".bak";
  if (!Storage.exists(backup.c_str())) return;
  if (Storage.exists(path.c_str())) {
    Storage.remove(backup.c_str());
  } else if (!Storage.rename(backup.c_str(), path.c_str())) {
    LOG_ERR("TRS", "Failed to recover TXT index backup");
  }
}

inline bool promotePageIndex(const std::string& temporary, const std::string& path) {
  recoverPageIndexBackup(path);
  if (!Storage.exists(path.c_str())) return Storage.rename(temporary.c_str(), path.c_str());
  const std::string backup = path + ".bak";
  if (!Storage.rename(path.c_str(), backup.c_str())) return false;
  if (Storage.rename(temporary.c_str(), path.c_str())) {
    Storage.remove(backup.c_str());
    return true;
  }
  if (!Storage.rename(backup.c_str(), path.c_str())) LOG_ERR("TRS", "Failed to restore TXT index backup");
  return false;
}

inline bool loadPageIndex(const std::string& path, const PageIndexSpec& spec, std::vector<size_t>& offsets) {
  recoverPageIndexBackup(path);
  HalFile file;
  if (!Storage.openFileForRead("TRS", path, file)) return false;
  PageIndexHeader header;
  const bool valid = readPageIndexHeader(file, header) && header.version == PAGE_INDEX_VERSION && header.spec == spec &&
                     validPageIndexLength(header, file.size());
  if (!valid) {
    file.close();
    return false;
  }
  offsets.clear();
  offsets.reserve(header.pageCount);
  for (uint32_t i = 0; i < header.pageCount; ++i) {
    uint32_t offset = 0;
    if (!serialization::tryReadPod(file, offset)) {
      offsets.clear();
      file.close();
      return false;
    }
    offsets.push_back(offset);
  }
  file.close();
  return true;
}

inline std::optional<size_t> readPageIndexOffset(const std::string& path, const PageIndexSpec& spec,
                                                 const uint16_t page) {
  recoverPageIndexBackup(path);
  HalFile file;
  if (!Storage.openFileForRead("TRS", path, file)) return std::nullopt;
  PageIndexHeader header;
  const bool valid = readPageIndexHeader(file, header) && header.version == PAGE_INDEX_VERSION && header.spec == spec &&
                     validPageIndexLength(header, file.size()) && page < header.pageCount;
  uint32_t offset = 0;
  const bool read = valid && file.seekSet(PAGE_INDEX_HEADER_BYTES + page * sizeof(uint32_t)) &&
                    serialization::tryReadPod(file, offset);
  file.close();
  if (!read || offset >= spec.sourceSize) return std::nullopt;
  return offset;
}

inline bool writePageIndex(HalFile& file, const PageIndexSpec& spec, const std::vector<size_t>& offsets) {
  if (!serialization::tryWritePodGroup(file, PAGE_INDEX_MAGIC, PAGE_INDEX_VERSION,
                                       static_cast<uint32_t>(spec.sourceSize), spec.viewportWidth, spec.linesPerPage,
                                       spec.fontId, spec.verticalMargin, spec.horizontalMargin, spec.paragraphAlignment,
                                       static_cast<uint32_t>(offsets.size()))) {
    return false;
  }
  for (const size_t offset : offsets) {
    if (!serialization::tryWritePod(file, static_cast<uint32_t>(offset))) return false;
  }
  return file.sync();
}

inline bool savePageIndex(const std::string& path, const PageIndexSpec& spec, const std::vector<size_t>& offsets,
                          const bool legacyMigrationComplete) {
  // A failed byte-anchor migration must never replace its only v4 source index.
  if (!legacyMigrationComplete) return false;
  if (offsets.empty() || offsets.size() > MAX_INDEX_PAGES) return false;
  recoverPageIndexBackup(path);
  const std::string temporary = path + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("TRS", temporary, file)) return false;
  const bool written = writePageIndex(file, spec, offsets);
  file.close();
  if (written && promotePageIndex(temporary, path)) return true;
  Storage.remove(temporary.c_str());
  return false;
}

inline bool saveProgressRecord(const std::string& cachePath, const uint16_t page, const uint32_t offset) {
  const uint8_t data[6] = {static_cast<uint8_t>(page),         static_cast<uint8_t>(page >> 8),
                           static_cast<uint8_t>(offset),       static_cast<uint8_t>(offset >> 8),
                           static_cast<uint8_t>(offset >> 16), static_cast<uint8_t>(offset >> 24)};
  HalFile file;
  if (!Storage.openFileForWrite("TRS", cachePath + "/progress.bin", file)) return false;
  const bool written = file.write(data, sizeof(data)) == sizeof(data);
  file.close();
  if (!written) LOG_ERR("TRS", "Short write saving reader progress");
  return written;
}

inline bool baseProgressAnchorMatches(HalFile& file, const uint16_t page, const uint8_t* progress,
                                      const bool hasByteOffset) {
  if (!hasByteOffset) return true;
  uint32_t offset = 0;
  return file.seekSet(PAGE_INDEX_HEADER_BYTES + page * sizeof(uint32_t)) && serialization::tryReadPod(file, offset) &&
         offset == readIndexUint32(progress + 2);
}

inline std::optional<uint32_t> readBaseProgressPageCount(const std::string& cachePath, const size_t sourceSize,
                                                         const uint8_t* progress, const bool hasByteOffset) {
  const std::string path = cachePath + "/index.bin";
  recoverPageIndexBackup(path);
  HalFile file;
  if (!Storage.openFileForRead("TRS", path, file)) return std::nullopt;
  PageIndexHeader header;
  const uint16_t page = static_cast<uint16_t>(progress[0] | (progress[1] << 8));
  // Home intentionally retains the existing v4-only percentage contract.
  const bool valid = readPageIndexHeader(file, header) && header.version == LEGACY_PAGE_INDEX_VERSION &&
                     header.spec.sourceSize == sourceSize && validPageIndexLength(header, file.size()) &&
                     page < header.pageCount && baseProgressAnchorMatches(file, page, progress, hasByteOffset);
  file.close();
  if (!valid) return std::nullopt;
  return header.pageCount;
}

inline std::optional<uint32_t> loadLegacyProgressPageCount(const std::string& cachePath, const size_t sourceSize,
                                                           uint16_t& page) {
  HalFile file;
  if (!Storage.openFileForRead("TRS", cachePath + "/progress.bin", file)) return std::nullopt;
  uint8_t progress[6];
  const int count = file.read(progress, sizeof(progress));
  file.close();
  if (count < 4) return std::nullopt;
  page = static_cast<uint16_t>(progress[0] | (progress[1] << 8));
  const bool hasByteOffset = count == sizeof(progress);
  if (hasByteOffset) {
    if (readIndexUint32(progress + 2) >= sourceSize) return std::nullopt;
    // A retained v4 base cannot identify a modern record's last layout when a
    // wide v5 slot exists. Never pair its page number with the legacy count.
    const std::string widePath = cachePath + "/index.wide.bin";
    if (Storage.exists(widePath.c_str()) || Storage.exists((widePath + ".bak").c_str()) ||
        Storage.exists((widePath + ".tmp").c_str())) {
      return std::nullopt;
    }
  }
  return readBaseProgressPageCount(cachePath, sourceSize, progress, hasByteOffset);
}

inline std::optional<size_t> readLegacyCacheOffset(const std::string& cachePath, const size_t sourceSize,
                                                   const uint16_t page) {
  // Page-only progress always belongs to the original index.bin, never the
  // selected geometry's slot (which may already have a completed wide index).
  const std::string path = cachePath + "/index.bin";
  recoverPageIndexBackup(path);
  HalFile file;
  if (!Storage.openFileForRead("TRS", path, file)) return std::nullopt;
  PageIndexHeader header;
  const bool valid = readPageIndexHeader(file, header) && header.version == LEGACY_PAGE_INDEX_VERSION &&
                     header.spec.sourceSize == sourceSize && validPageIndexLength(header, file.size()) &&
                     page < header.pageCount;
  uint32_t offset = 0;
  const bool read = valid && file.seekSet(PAGE_INDEX_HEADER_BYTES + page * sizeof(uint32_t)) &&
                    serialization::tryReadPod(file, offset);
  file.close();
  if (!read || offset >= sourceSize) return std::nullopt;
  return offset;
}

inline bool upgradeLegacyProgress(const std::string& cachePath, const size_t offset) {
  const std::string path = cachePath + "/progress.bin";
  HalFile progress = Storage.open(path.c_str(), O_WRONLY);
  if (!progress) return false;
  if (!progress.seekSet(2)) {
    progress.close();
    return false;
  }
  const uint8_t data[4] = {static_cast<uint8_t>(offset), static_cast<uint8_t>(offset >> 8),
                           static_cast<uint8_t>(offset >> 16), static_cast<uint8_t>(offset >> 24)};
  const bool persisted = progress.write(data, sizeof(data)) == sizeof(data) && progress.sync();
  progress.close();
  if (!persisted) LOG_ERR("TRS", "Failed to migrate legacy TXT progress");
  return persisted;
}

struct LegacyProgressMigration {
  // Modern progress passes its byte anchor through without another file read.
  std::optional<size_t> offset;
  bool replacementSafe = true;
};

inline LegacyProgressMigration migrateLegacyProgress(const std::string& cachePath, const size_t sourceSize) {
  const std::string path = cachePath + "/progress.bin";
  HalFile progress;
  if (!Storage.openFileForRead("TRS", path, progress)) {
    return {std::nullopt, !Storage.exists(path.c_str())};
  }
  uint8_t data[6];
  const int count = progress.read(data, sizeof(data));
  progress.close();
  if (count < 4) return {};
  if (count == sizeof(data)) {
    const size_t offset = readIndexUint32(data + 2);
    if (offset < sourceSize) return {offset, true};
    return {};
  }
  // Five bytes can be an interrupted extension of the original four bytes.
  const auto offset = readLegacyCacheOffset(cachePath, sourceSize, static_cast<uint16_t>(data[0] | (data[1] << 8)));
  if (!offset) {
    // An unreadable legacy source must survive too, not just a failed upgrade.
    return {std::nullopt, !Storage.exists((cachePath + "/index.bin").c_str())};
  }
  return {offset, upgradeLegacyProgress(cachePath, *offset)};
}
}  // namespace txtlayout
