#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

#include "lib/EpdFont/EpdFont.h"
#include "lib/EpdFont/EpdFontData.h"
#include "lib/Utf8/Utf8.h"

namespace {
constexpr uint8_t kClassCount = 3;
constexpr int8_t kDenseMatrix[] = {-5, 0, 7, -9, -11, 0, 0, 13, -17};
constexpr uint16_t kRowOffsets[] = {0, 2, 4, 6};
constexpr uint8_t kSparseColumns[] = {0, 2, 0, 1, 1, 2};
constexpr int8_t kSparseValues[] = {-5, 7, -9, -11, 13, -17};

constexpr std::array<uint8_t, 256> kAsciiClasses = [] {
  std::array<uint8_t, 256> classes{};
  classes[0] = 1;
  classes['A'] = 1;
  classes['B'] = 2;
  classes['D'] = 3;
  classes[0x7F] = 2;
  classes[128] = 3;
  classes[128 + 'A'] = 2;
  classes[128 + 'B'] = 3;
  classes[128 + 'C'] = 1;
  classes[128 + 0x7F] = 1;
  return classes;
}();

struct ClassMap {
  std::vector<EpdKernClassEntry> packed;
  std::vector<uint16_t> codepoints;
  std::vector<uint8_t> classIds;

  void assign(std::vector<EpdKernClassEntry> entries) {
    packed = std::move(entries);
    codepoints.clear();
    classIds.clear();
    codepoints.reserve(packed.size());
    classIds.reserve(packed.size());
    for (const auto& entry : packed) {
      codepoints.push_back(static_cast<uint16_t>(entry.codepoint));
      classIds.push_back(entry.classId);
    }
  }
};

// Deliberately scan the complete fixture: the oracle does not share the
// optimized search bound, and duplicate keys must resolve to their first class.
uint8_t referenceClass(const ClassMap& map, const uint32_t cp) {
  if (cp > 0xFFFF) return 0;
  for (const auto& entry : map.packed) {
    if (entry.codepoint == cp) return entry.classId;
  }
  return 0;
}

int8_t referenceKerning(const ClassMap& leftMap, const ClassMap& rightMap, const uint32_t left, const uint32_t right) {
  if (utf8IsCjkBreakable(left) || utf8IsCjkBreakable(right)) return 0;
  const uint8_t leftClass = referenceClass(leftMap, left);
  const uint8_t rightClass = referenceClass(rightMap, right);
  if (leftClass == 0 || rightClass == 0) return 0;
  return kDenseMatrix[(leftClass - 1) * kClassCount + rightClass - 1];
}

uint32_t nextRandom(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return state;
}

// Callers use at most 128 entries starting at U+FC00 or lower; even the
// maximum step stays in the BMP. Zero steps intentionally generate duplicates.
std::vector<EpdKernClassEntry> makeSortedMap(const size_t count, const uint16_t first, uint32_t& state) {
  std::vector<EpdKernClassEntry> entries;
  entries.reserve(count);
  uint32_t cp = first;
  for (size_t i = 0; i < count; ++i) {
    const auto classId = static_cast<uint8_t>((nextRandom(state) >> 16) % 4);
    entries.push_back({static_cast<uint16_t>(cp), classId});
    cp += (nextRandom(state) >> 16) % 8;
  }
  return entries;
}

// Parameters are (split class maps, sparse matrix). Every fixture exercises all
// four combinations through the real EpdFont::getKerning consumer API.
class KerningLookupTest : public testing::TestWithParam<std::tuple<bool, bool>> {
 protected:
  void configure(std::vector<EpdKernClassEntry> left, std::vector<EpdKernClassEntry> right) {
    left_.assign(std::move(left));
    right_.assign(std::move(right));
    data_ = EpdFontData{};
    data_.kernLeftEntryCount = static_cast<uint16_t>(left_.packed.size());
    data_.kernRightEntryCount = static_cast<uint16_t>(right_.packed.size());
    data_.kernLeftClassCount = kClassCount;
    data_.kernRightClassCount = kClassCount;
    if (std::get<0>(GetParam())) {
      data_.kernLeftCodepoints = left_.codepoints.data();
      data_.kernLeftClassIds = left_.classIds.data();
      data_.kernRightCodepoints = right_.codepoints.data();
      data_.kernRightClassIds = right_.classIds.data();
    } else {
      data_.kernLeftClasses = left_.packed.data();
      data_.kernRightClasses = right_.packed.data();
    }
    if (std::get<1>(GetParam())) {
      data_.kernRowOffsets = kRowOffsets;
      data_.kernSparseCols = kSparseColumns;
      data_.kernSparseValues = kSparseValues;
    } else {
      data_.kernMatrix = kDenseMatrix;
    }
  }

  void configureAscii() {
    configure({{0, 1}, {'A', 1}, {'B', 2}, {'C', 0}, {'D', 3}, {0x7F, 2}, {0x80, 3}, {0x100, 2}, {0xFFFF, 3}},
              {{0, 3}, {'A', 2}, {'B', 3}, {'C', 1}, {'D', 0}, {0x7F, 1}, {0x80, 2}, {0x100, 3}, {0xFFFF, 2}});
    data_.kernAsciiClassIds = kAsciiClasses.data();
  }

  EpdFont font() const { return EpdFont(&data_); }

  void expectPair(const uint32_t left, const uint32_t right) const {
    EXPECT_EQ(static_cast<int>(font().getKerning(left, right)),
              static_cast<int>(referenceKerning(left_, right_, left, right)))
        << "left=" << left << ", right=" << right;
  }

  ClassMap left_;
  ClassMap right_;
  EpdFontData data_{};
};

TEST_P(KerningLookupTest, EmptyMapsReturnZero) {
  configure({}, {{0x41, 1}});
  for (const uint32_t cp : {0u, 0x40u, 0x41u, 0xFFFFu, 0x10000u, 0xFFFFFFFFu}) {
    expectPair(cp, 0x41);
  }
  configure({{0x41, 1}}, {});
  for (const uint32_t cp : {0u, 0x40u, 0x41u, 0xFFFFu, 0x10000u, 0xFFFFFFFFu}) {
    expectPair(0x41, cp);
  }
  configure({}, {});
  for (const uint32_t cp : {0u, 0x41u, 0xFFFFu, 0x10000u}) {
    expectPair(cp, cp);
  }
}

TEST_P(KerningLookupTest, ZeroCountsWithNonNullStorageReturnZero) {
  configure({{0x41, 1}}, {{0x42, 1}});
  data_.kernLeftEntryCount = 0;
  EXPECT_EQ(font().getKerning(0x41, 0x42), 0);
  data_.kernLeftEntryCount = 1;
  data_.kernRightEntryCount = 0;
  EXPECT_EQ(font().getKerning(0x41, 0x42), 0);
  data_.kernLeftEntryCount = 0;
  EXPECT_EQ(font().getKerning(0x41, 0x42), 0);
}

TEST_P(KerningLookupTest, MissingMapPointersReturnZero) {
  configure({{0x41, 1}}, {{0x42, 1}});
  const EpdFontData complete = data_;
  data_.kernLeftClasses = nullptr;
  data_.kernLeftCodepoints = nullptr;
  EXPECT_EQ(font().getKerning(0x41, 0x42), 0);
  data_ = complete;
  data_.kernRightClasses = nullptr;
  data_.kernRightCodepoints = nullptr;
  EXPECT_EQ(font().getKerning(0x41, 0x42), 0);
  if (std::get<0>(GetParam())) {
    data_ = complete;
    data_.kernLeftClassIds = nullptr;
    EXPECT_EQ(font().getKerning(0x41, 0x42), 0);
    data_ = complete;
    data_.kernRightClassIds = nullptr;
    EXPECT_EQ(font().getKerning(0x41, 0x42), 0);
  }
}

TEST_P(KerningLookupTest, SingletonAtMaximumBmpCodepointDoesNotWrap) {
  configure({{0xFFFF, 3}}, {{0xFFFF, 3}});
  constexpr uint32_t queries[] = {0, 0xFFFE, 0xFFFF, 0x10000, 0x1FFFF, 0x10FFFF, 0xFFFFFFFF};
  for (const uint32_t left : queries) {
    for (const uint32_t right : queries) {
      expectPair(left, right);
    }
  }
  EXPECT_EQ(font().getKerning(0xFFFF, 0xFFFF), -17);
}

TEST_P(KerningLookupTest, EntireBmpSpanAndMissingInteriorKeysMatchTheReference) {
  configure({{0, 1}, {0x20, 2}, {0xFFFF, 3}}, {{0, 3}, {0x41, 1}, {0xFFFF, 2}});
  constexpr uint32_t queries[] = {0,      1,      0x1F,   0x20,    0x21,    0x40,     0x41,      0x42,
                                  0xFFFD, 0xFFFE, 0xFFFF, 0x10000, 0x1FFFF, 0x10FFFF, 0xFFFFFFFF};
  for (const uint32_t left : queries) {
    for (const uint32_t right : queries) {
      expectPair(left, right);
    }
  }
}

TEST_P(KerningLookupTest, SparseUniqueMapsMatchAcrossWideKeyGaps) {
  configure({{0x20, 1}, {0x41, 2}, {0x3A9, 3}, {0x2000, 1}, {0xFEF0, 2}, {0xFFFF, 3}},
            {{0x21, 3}, {0x56, 1}, {0x3B1, 2}, {0x2009, 1}, {0xFEFF, 3}, {0xFFFF, 2}});
  constexpr uint32_t queries[] = {0,    0x1F,  0x20,  0x21,   0x22,   0x40,   0x41,   0x42,   0x55,   0x56,
                                  0x57, 0x3A9, 0x3B1, 0x2000, 0x2009, 0xFEF0, 0xFEFF, 0xFFFE, 0xFFFF, 0x10000};
  for (const uint32_t left : queries) {
    for (const uint32_t right : queries) {
      expectPair(left, right);
    }
  }
}

TEST_P(KerningLookupTest, DenseUniqueMapsMatchAtEveryKeyAndAdjacentMisses) {
  std::vector<EpdKernClassEntry> left;
  std::vector<EpdKernClassEntry> right;
  left.reserve(1024);
  right.reserve(1024);
  for (uint32_t i = 0; i < 1024; ++i) {
    const auto cp = static_cast<uint16_t>(0x20 + i);
    left.push_back({cp, static_cast<uint8_t>(1 + i % kClassCount)});
    right.push_back({cp, static_cast<uint8_t>(1 + (i + 1) % kClassCount)});
  }
  configure(std::move(left), std::move(right));
  for (uint32_t cp = 0x1F; cp <= 0x420; ++cp) {
    expectPair(cp, 0x41);
    expectPair(0x41, cp);
  }
}

TEST_P(KerningLookupTest, DuplicateKeysRetainFirstClassWhenPrefixCannotReachTarget) {
  // For U+0041 the numeric prefix has length two, but ends at U+0040:
  // shortening without checking that actual key would lose a present target.
  // U+0043 also verifies that a first duplicate with class zero stays missing.
  configure({{0x40, 1},
             {0x40, 2},
             {0x41, 3},
             {0x41, 1},
             {0x41, 2},
             {0x42, 2},
             {0x43, 0},
             {0x43, 3},
             {0x44, 3},
             {0xFFFE, 2},
             {0xFFFF, 1},
             {0xFFFF, 3}},
            {{0x40, 3},
             {0x40, 2},
             {0x41, 2},
             {0x41, 1},
             {0x42, 1},
             {0x43, 0},
             {0x43, 2},
             {0x44, 3},
             {0xFFFE, 1},
             {0xFFFF, 3},
             {0xFFFF, 2}});
  constexpr uint32_t queries[] = {0, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0xFFFD, 0xFFFE, 0xFFFF, 0x10000};
  for (const uint32_t left : queries) {
    for (const uint32_t right : queries) {
      expectPair(left, right);
    }
  }
  EXPECT_EQ(font().getKerning(0x41, 0x41), 13);
  EXPECT_EQ(font().getKerning(0x43, 0x40), 0);
  EXPECT_EQ(font().getKerning(0x40, 0x43), 0);
}

TEST_P(KerningLookupTest, MaximumEntryCountKeepsWideIndexAndCodepointArithmetic) {
  std::vector<EpdKernClassEntry> left;
  std::vector<EpdKernClassEntry> right;
  left.reserve(0xFFFF);
  right.reserve(0xFFFF);
  for (uint32_t i = 0; i < 0xFFFF; ++i) {
    left.push_back({static_cast<uint16_t>(i), static_cast<uint8_t>(1 + i % kClassCount)});
    right.push_back({static_cast<uint16_t>(i + 1), static_cast<uint8_t>(1 + (i + 1) % kClassCount)});
  }
  configure(std::move(left), std::move(right));
  constexpr uint32_t queries[] = {0,      1,      2,      0x20,   0x41,   0x1000, 0x10FF, 0x1100, 0x11FF,
                                  0x1200, 0x3000, 0x8000, 0xFF00, 0xFFF0, 0xFFFD, 0xFFFE, 0xFFFF, 0x10000};
  for (const uint32_t cp : queries) {
    expectPair(cp, 0x41);
    expectPair(0x41, cp);
    expectPair(cp, cp);
  }
}

TEST_P(KerningLookupTest, BoundedDeterministicDuplicateMapsMatchAroundEveryKey) {
  uint32_t state = 0xC0FFEE;
  for (size_t run = 0; run < 16; ++run) {
    SCOPED_TRACE(run);
    const size_t leftCount = 1 + (nextRandom(state) >> 16) % 128;
    const size_t rightCount = 1 + (nextRandom(state) >> 16) % 128;
    auto left = makeSortedMap(leftCount, 0x20, state);
    auto right = makeSortedMap(rightCount, 0xFC00, state);
    configure(std::move(left), std::move(right));
    for (size_t i = 0; i < left_.packed.size(); ++i) {
      const uint32_t rightCp = right_.packed[i % right_.packed.size()].codepoint;
      for (const int delta : {-1, 0, 1}) {
        const auto leftCp = static_cast<uint32_t>(static_cast<int>(left_.packed[i].codepoint) + delta);
        expectPair(leftCp, rightCp);
      }
    }
    for (size_t i = 0; i < right_.packed.size(); ++i) {
      const uint32_t leftCp = left_.packed[i % left_.packed.size()].codepoint;
      for (const int delta : {-1, 0, 1}) {
        const auto rightCp = static_cast<uint32_t>(static_cast<int>(right_.packed[i].codepoint) + delta);
        expectPair(leftCp, rightCp);
      }
    }
  }
}

TEST_P(KerningLookupTest, AsciiClassesRetainAsymmetricPairsZeroClassesAndMisses) {
  configureAscii();
  for (uint32_t left = 0; left < 128; ++left) {
    for (uint32_t right = 0; right < 128; ++right) {
      expectPair(left, right);
    }
  }
  EXPECT_EQ(font().getKerning('A', 'B'), 7);
  EXPECT_EQ(font().getKerning('B', 'A'), -11);
  EXPECT_EQ(font().getKerning('D', 'A'), 13);
  EXPECT_EQ(font().getKerning('A', 'C'), -5);
  EXPECT_EQ(font().getKerning('A', 'A'), 0);  // Present classes, zero matrix cell.
  EXPECT_EQ(font().getKerning('C', 'A'), 0);  // Explicit zero left class.
  EXPECT_EQ(font().getKerning('A', 'D'), 0);  // Explicit zero right class.
  EXPECT_EQ(font().getKerning('Z', 'A'), 0);  // Absent left codepoint.
  EXPECT_EQ(font().getKerning('A', 'Z'), 0);  // Absent right codepoint.
}

TEST_P(KerningLookupTest, AsciiEndpointsAndNonAsciiFallbackDoNotAlias) {
  configureAscii();
  constexpr uint32_t queries[] = {0,    1,     'A',   'B',    0x7E,    0x7F,     0x80,
                                  0x81, 0x100, 0x101, 0xFFFF, 0x10000, 0x10FFFF, 0xFFFFFFFF};
  for (const uint32_t left : queries) {
    for (const uint32_t right : queries) {
      expectPair(left, right);
    }
  }
  EXPECT_EQ(font().getKerning(0, 0), 7);
  EXPECT_EQ(font().getKerning(0x7F, 0x7F), -9);
  EXPECT_EQ(font().getKerning(0x7F, 0x80), -11);
  EXPECT_EQ(font().getKerning(0x80, 0x7F), 0);
  EXPECT_EQ(font().getKerning(0x80, 0x80), 13);
  EXPECT_EQ(font().getKerning(0x80, 0x100), -17);
  EXPECT_EQ(font().getKerning(0xFFFF, 0xFFFF), 13);
}

TEST_P(KerningLookupTest, NonAsciiDuplicatesKeepFirstClassWithAsciiClassesPresent) {
  configure({{'A', 1}, {0x80, 3}, {0x80, 2}, {0x100, 0}, {0x100, 1}, {0xFFFF, 3}, {0xFFFF, 1}},
            {{'A', 2}, {0x80, 2}, {0x80, 3}, {0x100, 0}, {0x100, 3}, {0xFFFF, 2}, {0xFFFF, 1}});
  data_.kernAsciiClassIds = kAsciiClasses.data();
  constexpr uint32_t queries[] = {'A', 0x80, 0x81, 0xFF, 0x100, 0x101, 0xFFFE, 0xFFFF, 0x10000};
  for (const uint32_t cp : queries) {
    expectPair(cp, 'A');
    expectPair('A', cp);
    expectPair(cp, cp);
  }
  EXPECT_EQ(font().getKerning(0x80, 0x80), 13);
  EXPECT_EQ(font().getKerning(0x100, 'A'), 0);
  EXPECT_EQ(font().getKerning('A', 0x100), 0);
}

TEST_P(KerningLookupTest, CjkGuardRemainsAheadOfAsciiAndFallbackClasses) {
  configure({{'A', 1}, {0x4E00, 3}}, {{'A', 2}, {0x4E00, 1}});
  data_.kernAsciiClassIds = kAsciiClasses.data();
  EXPECT_EQ(font().getKerning(0x4E00, 'A'), 0);
  EXPECT_EQ(font().getKerning('A', 0x4E00), 0);
  EXPECT_EQ(font().getKerning(0x4E00, 0x4E00), 0);
}

#if CROSSINK_SCALABLE_FONTS
TEST_P(KerningLookupTest, ScalableKerningOverridesStaticClassesAfterTheCjkGuard) {
  configureAscii();
  int calls = 0;
  data_.glyphMissCtx = &calls;
  data_.kerningHandler = [](void* ctx, uint32_t, uint32_t) -> int8_t {
    ++*static_cast<int*>(ctx);
    return 23;
  };
  EXPECT_EQ(font().getKerning('A', 'B'), 23);
  EXPECT_EQ(font().getKerning('Z', 'Z'), 23);
  EXPECT_EQ(font().getKerning(0x80, 0x80), 23);
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(font().getKerning(0x4E00, 'A'), 0);
  EXPECT_EQ(font().getKerning('A', 0x4E00), 0);
  EXPECT_EQ(calls, 3);
}
#endif

INSTANTIATE_TEST_SUITE_P(MapAndMatrixRepresentations, KerningLookupTest,
                         testing::Combine(testing::Bool(), testing::Bool()));
}  // namespace
