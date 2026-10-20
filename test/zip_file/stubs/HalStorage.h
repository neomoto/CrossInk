#pragma once

#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct ZipHostFileData {
  std::vector<uint8_t> bytes;
  size_t readFaultOffset = 0;
  int readFaultResult = 0;
  bool readFaultPending = false;
  size_t seekFaultOffset = 0;
  bool seekFaultPending = false;
};

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::shared_ptr<ZipHostFileData> data) : data_(std::move(data)) {}

  size_t size() const { return data_ ? data_->bytes.size() : 0; }
  size_t position() const { return cursor_; }
  int available() const { return data_ && cursor_ < data_->bytes.size(); }

  int read(void* output, const size_t length) {
    if (!data_) return -1;
    size_t limit = length;
    if (data_->readFaultPending && cursor_ == data_->readFaultOffset) {
      data_->readFaultPending = false;
      if (data_->readFaultResult < 0) return data_->readFaultResult;
      limit = std::min(limit, static_cast<size_t>(data_->readFaultResult));
    }
    const size_t readable = std::min(limit, data_->bytes.size() - std::min(cursor_, data_->bytes.size()));
    if (readable > 0) std::copy_n(data_->bytes.data() + cursor_, readable, static_cast<uint8_t*>(output));
    cursor_ += readable;
    return static_cast<int>(readable);
  }

  bool seek(const size_t position) {
    if (!data_) return false;
    if (data_->seekFaultPending && position == data_->seekFaultOffset) {
      data_->seekFaultPending = false;
      return false;
    }
    if (position > data_->bytes.size()) return false;
    cursor_ = position;
    return true;
  }
  bool seekCur(const int64_t offset) {
    const int64_t position = static_cast<int64_t>(cursor_) + offset;
    return position >= 0 && seek(static_cast<size_t>(position));
  }
  void close() {
    data_.reset();
    cursor_ = 0;
  }
  explicit operator bool() const { return static_cast<bool>(data_); }

 private:
  std::shared_ptr<ZipHostFileData> data_;
  size_t cursor_ = 0;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  void reset() { files_.clear(); }
  void put(const std::string& path, std::vector<uint8_t> bytes) {
    auto data = std::make_shared<ZipHostFileData>();
    data->bytes = std::move(bytes);
    files_[path] = std::move(data);
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    const auto it = files_.find(path);
    if (it == files_.end()) return false;
    file = HalFile(it->second);
    return true;
  }
  void failReadOnce(const std::string& path, const size_t offset, const int result) {
    auto& data = *files_.at(path);
    data.readFaultOffset = offset;
    data.readFaultResult = result;
    data.readFaultPending = true;
  }
  void failSeekOnce(const std::string& path, const size_t offset) {
    auto& data = *files_.at(path);
    data.seekFaultOffset = offset;
    data.seekFaultPending = true;
  }
  void overwrite(const std::string& path, const size_t offset, const std::vector<uint8_t>& bytes) {
    auto& destination = files_.at(path)->bytes;
    std::copy(bytes.begin(), bytes.end(), destination.begin() + static_cast<std::ptrdiff_t>(offset));
  }

 private:
  std::unordered_map<std::string, std::shared_ptr<ZipHostFileData>> files_;
};

#define Storage HalStorage::getInstance()
