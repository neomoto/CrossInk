#pragma once

#include <HalStorage.h>
#include <Serialization.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Only optional idle reads use this budget. The ordinary requested-page path
// keeps its existing deserializer and limits. Charge BEFORE each allocation in
// that same deserializer, so changed/corrupt nested lengths cannot evade a
// separate preflight pass. No serialized-page copy or large stack buffer exists.
class PageReadBudget {
 public:
  static constexpr size_t MAX_SERIALIZED_BYTES = 8U * 1024U;
  static constexpr size_t MAX_ALLOCATION_CHARGE = 16U * 1024U;
  // Conservative accounting, not a measurement of physical heap consumption:
  // round every payload to 16 bytes and allow 64 bytes for allocator overhead.
  // A unique_ptr -> shared_ptr conversion additionally charges a 64-byte
  // control-block payload (and its own allocation overhead).
  static constexpr size_t ALLOCATION_ALLOWANCE = 64;
  static constexpr size_t SHARED_CONTROL_BLOCK_BYTES = 64;

  PageReadBudget(const size_t start, const size_t end, const size_t allocationLimit = MAX_ALLOCATION_CHARGE)
      : start_(start), end_(end), limit_(std::min(allocationLimit, MAX_ALLOCATION_CHARGE)) {}

  bool valid() const { return end_ > start_ && end_ - start_ <= MAX_SERIALIZED_BYTES; }
  bool allowsRead(const FsFile& file, const size_t bytes) const {
    const size_t position = file.position();
    return position >= start_ && position <= end_ && bytes <= end_ - position;
  }
  bool charge(const size_t bytes) {
    if (bytes == 0) return true;
    if (bytes > MAX_ALLOCATION_CHARGE) return false;
    const size_t charge = ((bytes + 15U) & ~size_t{15U}) + ALLOCATION_ALLOWANCE;
    if (charge > limit_ - charged_) return false;
    charged_ += charge;
    return true;
  }
  size_t chargedBytes() const { return charged_; }

 private:
  const size_t start_;
  const size_t end_;
  const size_t limit_;
  size_t charged_ = 0;
};

inline bool chargePageAllocation(PageReadBudget* budget, const size_t bytes) {
  return !budget || budget->charge(bytes);
}

inline bool readPageBytes(FsFile& file, PageReadBudget* budget, void* output, const size_t bytes) {
  if (budget && !budget->allowsRead(file, bytes)) return false;
  return file.read(output, bytes) == static_cast<int>(bytes);
}

template <typename T>
bool unpackPageField(const uint8_t* bytes, size_t& offset, T& value) {
  std::memcpy(&value, bytes + offset, sizeof(T));
  offset += sizeof(T);
  return true;
}

inline bool unpackPageField(const uint8_t* bytes, size_t& offset, bool& value) {
  static_assert(sizeof(bool) == 1, "Page wire booleans require a one-byte representation");
  const uint8_t wireValue = bytes[offset++];
  if (wireValue > 1) return false;
  value = wireValue != 0;
  return true;
}

template <typename... Ts>
bool readPagePodGroup(FsFile& file, PageReadBudget* budget, Ts&... values) {
  if (!budget) return serialization::tryReadPodGroup(file, values...);
  constexpr size_t byteCount = (sizeof(Ts) + ...);
  static_assert(byteCount <= 64, "Use direct reads for larger payloads");
  uint8_t bytes[byteCount];
  if (!readPageBytes(file, budget, bytes, byteCount)) return false;
  size_t offset = 0;
  return (unpackPageField(bytes, offset, values) && ...);
}

inline bool readPageString(FsFile& file, PageReadBudget* budget, std::string& value) {
  if (!budget) return serialization::tryReadString(file, value);
  uint32_t length = 0;
  if (!readPagePodGroup(file, budget, length) || !budget->allowsRead(file, length)) return false;
  // Default std::string growth may choose its minimum capacity for short
  // strings. Charge at least 32 bytes, even when small-string storage suffices.
  if (!budget->charge(std::max<size_t>(32, static_cast<size_t>(length) + 1U))) return false;
  value.resize(length);
  return length == 0 || readPageBytes(file, budget, &value[0], length);
}
