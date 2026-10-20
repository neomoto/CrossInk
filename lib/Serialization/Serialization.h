#pragma once
#include <HalStorage.h>

#include <cstring>
#include <iostream>
#include <limits>
#include <type_traits>

namespace serialization {
template <typename T>
static void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
static void writePod(FsFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
static bool tryWritePod(FsFile& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

// Concatenate scalar fields without struct padding or a deferred flush. Keep
// these short metadata records on the stack; large payloads are written directly.
template <typename... Ts>
static bool tryWritePodGroup(FsFile& file, const Ts&... values) {
  static_assert((std::is_trivially_copyable<Ts>::value && ...), "Fields must be trivially copyable");
  constexpr size_t byteCount = (sizeof(Ts) + ...);
  static_assert(byteCount <= 64, "Use direct writes for larger payloads");
  uint8_t bytes[byteCount];
  size_t offset = 0;
  const auto append = [&](const auto& value) {
    std::memcpy(bytes + offset, &value, sizeof(value));
    offset += sizeof(value);
  };
  (append(values), ...);
  return file.write(bytes, byteCount) == byteCount;
}

template <typename T>
static void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
static void readPod(FsFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

template <typename T>
static bool tryReadPod(FsFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

// Decode only complete short records, leaving every output unchanged on a short
// read. Copy fields individually so wire bytes never depend on struct padding.
template <typename... Ts>
static bool tryReadPodGroup(FsFile& file, Ts&... values) {
  static_assert((std::is_trivially_copyable<Ts>::value && ...), "Fields must be trivially copyable");
  constexpr size_t byteCount = (sizeof(Ts) + ...);
  static_assert(byteCount <= 64, "Use direct reads for larger payloads");
  uint8_t bytes[byteCount];
  if (file.read(bytes, byteCount) != static_cast<int>(byteCount)) {
    return false;
  }
  size_t offset = 0;
  const auto extract = [&](auto& value) {
    std::memcpy(&value, bytes + offset, sizeof(value));
    offset += sizeof(value);
  };
  (extract(values), ...);
  return true;
}

static void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

static void writeString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

static bool tryWriteString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  return tryWritePod(file, len) && (len == 0 || file.write(reinterpret_cast<const uint8_t*>(s.data()), len) == len);
}

static void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  s.resize(len);
  is.read(&s[0], len);
}

static void readString(FsFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  s.resize(len);
  file.read(&s[0], len);
}

static bool tryReadString(FsFile& file, std::string& s) {
  uint32_t len = 0;
  if (!tryReadPod(file, len)) {
    return false;
  }
  if (static_cast<size_t>(len) > s.max_size() || len > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  s.resize(len);
  const int readLen = static_cast<int>(len);
  return len == 0 || file.read(&s[0], readLen) == readLen;
}
}  // namespace serialization
