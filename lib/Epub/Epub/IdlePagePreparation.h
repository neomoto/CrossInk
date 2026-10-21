#pragma once

#include <memory>
#include <tuple>

#include "Page.h"
#include "ReaderRenderSpec.h"

struct IdlePageKey {
  const void* book = nullptr;
  const void* section = nullptr;
  uint32_t sectionGeneration = 0;
  int spine = -1;
  int page = -1;
  uint32_t fontGeneration = 0;
  ReaderRenderSpec layout;

  bool operator==(const IdlePageKey& other) const {
    return std::tie(book, section, sectionGeneration, spine, page, fontGeneration) ==
               std::tie(other.book, other.section, other.sectionGeneration, other.spine, other.page,
                        other.fontGeneration) &&
           readerRenderSpecsEqual(layout, other.layout);
  }
};

// One exact successor, never a second render bitmap or a current-page cache.
// Lifecycle/consume-once design adapted from CrossDiTo (MIT), commit
// 4506e2e9516dc1330695cb6f4c337e7d12a7fe55. X3 admission and bounded decoding
// are deliberately stricter than the PSRAM reader's page-count-only cache.
class IdlePagePreparation {
 public:
  static constexpr uint32_t MIN_PREPARE_FREE = 80U * 1024U;
  static constexpr uint32_t MIN_PREPARE_CONTIGUOUS = 56U * 1024U;
  static constexpr uint32_t MIN_RETAIN_FREE = 64U * 1024U;
  static constexpr uint32_t MIN_RETAIN_CONTIGUOUS = 40U * 1024U;

  static bool canPrepare(const uint32_t free, const uint32_t contiguous) {
    return free >= MIN_PREPARE_FREE && contiguous >= MIN_PREPARE_CONTIGUOUS;
  }
  static bool canRetain(const uint32_t free, const uint32_t contiguous) {
    return free >= MIN_RETAIN_FREE && contiguous >= MIN_RETAIN_CONTIGUOUS;
  }

  bool empty() const { return page_ == nullptr; }
  void clear() { page_.reset(); }
  template <typename AcquireLock>
  void beforePageTurn(const bool isForwardTurn, AcquireLock&& acquireLock) {
    // Forward navigation may consume the exact successor. Backward navigation
    // invalidates ownership now, under the reader mutex, not after a redraw.
    // Acquire lazily so a forward turn gains no new lock/wait on this cache.
    if (isForwardTurn) return;
    auto navigationLock = acquireLock();
    clear();
  }
  void discardUnderPressure(const uint32_t free, const uint32_t contiguous) {
    if (!canRetain(free, contiguous)) clear();
  }
  bool retain(std::unique_ptr<Page> page, const IdlePageKey& key, const uint32_t free, const uint32_t contiguous) {
    clear();
    if (!page || !canRetain(free, contiguous)) return false;
    key_ = key;
    page_ = std::move(page);
    return true;
  }
  std::unique_ptr<Page> take(const IdlePageKey& requested, const uint32_t free, const uint32_t contiguous) {
    // Always detach first: any miss (including a current-page redraw) destroys
    // the speculative page BEFORE the caller allocates a foreground page.
    auto page = std::move(page_);
    if (!(key_ == requested) || !canRetain(free, contiguous)) return nullptr;
    return page;
  }
  template <typename LoadRequestedPage>
  std::unique_ptr<Page> load(const IdlePageKey& requested, const uint32_t free, const uint32_t contiguous,
                             LoadRequestedPage&& loadRequestedPage) {
    auto page = take(requested, free, contiguous);
    if (page) return page;
    // take() has already destroyed every nonmatching/pressured backing object.
    // The foreground loader must never overlap with that optional residency.
    return loadRequestedPage();
  }

 private:
  IdlePageKey key_;
  std::unique_ptr<Page> page_;
};
