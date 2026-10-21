#include <Bitmap.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {
void writeLe16(std::vector<uint8_t>& data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(std::vector<uint8_t>& data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

std::vector<uint8_t> create24BitBmp(const int width, const int height, const bool topDown = false) {
  const int rowBytes = (width * 24 + 31) / 32 * 4;
  constexpr size_t kPixelOffset = 54;
  std::vector<uint8_t> data(kPixelOffset + static_cast<size_t>(rowBytes) * height, 0);
  data[0] = 'B';
  data[1] = 'M';
  writeLe32(data, 2, static_cast<uint32_t>(data.size()));
  writeLe32(data, 10, kPixelOffset);
  writeLe32(data, 14, 40);
  writeLe32(data, 18, width);
  writeLe32(data, 22, static_cast<uint32_t>(topDown ? -height : height));
  writeLe16(data, 26, 1);
  writeLe16(data, 28, 24);
  writeLe32(data, 34, static_cast<uint32_t>(rowBytes * height));

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const uint8_t value = static_cast<uint8_t>((x * 13 + y * 7) & 0xFF);
      const int fileRow = topDown ? y : height - 1 - y;
      const size_t offset = kPixelOffset + static_cast<size_t>(fileRow) * rowBytes + x * 3;
      data[offset] = value;
      data[offset + 1] = value;
      data[offset + 2] = value;
    }
  }
  return data;
}

std::vector<uint8_t> create2BitBmp(const int width, const std::vector<uint8_t>& palette,
                                   const std::vector<std::vector<uint8_t>>& rows, const bool topDown = false,
                                   const bool defaultColorsUsed = false) {
  const int height = static_cast<int>(rows.size());
  const int rowBytes = (width * 2 + 31) / 32 * 4;
  const size_t pixelOffset = 54 + palette.size() * 4;
  std::vector<uint8_t> data(pixelOffset + static_cast<size_t>(rowBytes) * height, 0);
  data[0] = 'B';
  data[1] = 'M';
  writeLe32(data, 2, static_cast<uint32_t>(data.size()));
  writeLe32(data, 10, static_cast<uint32_t>(pixelOffset));
  writeLe32(data, 14, 40);
  writeLe32(data, 18, width);
  writeLe32(data, 22, static_cast<uint32_t>(topDown ? -height : height));
  writeLe16(data, 26, 1);
  writeLe16(data, 28, 2);
  writeLe32(data, 34, static_cast<uint32_t>(rowBytes * height));
  writeLe32(data, 46, defaultColorsUsed ? 0 : static_cast<uint32_t>(palette.size()));
  for (size_t i = 0; i < palette.size(); i++) {
    data[54 + i * 4] = data[55 + i * 4] = data[56 + i * 4] = palette[i];
  }
  // Deliberately dirty BMP padding; only the supplied packed pixels belong in output.
  std::fill(data.begin() + pixelOffset, data.end(), 0xA5);
  for (int y = 0; y < height; y++) {
    const int fileRow = topDown ? y : height - 1 - y;
    std::copy(rows[y].begin(), rows[y].end(), data.begin() + pixelOffset + static_cast<size_t>(fileRow) * rowBytes);
  }
  return data;
}

uint32_t fingerprint(const std::vector<uint8_t>& data) {
  uint32_t hash = 2166136261U;
  for (const uint8_t value : data) {
    hash = (hash ^ value) * 16777619U;
  }
  return hash;
}
}  // namespace

TEST(BitmapResample, DownsamplesBeforeDitheringAndRewindsDeterministically) {
  HalFile file(create24BitBmp(480, 800));
  Bitmap bitmap(file, true);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  ASSERT_TRUE(bitmap.setDitheredOutputSize(475, 792));
  EXPECT_EQ(bitmap.getWidth(), 475);
  EXPECT_EQ(bitmap.getHeight(), 792);

  std::vector<uint8_t> firstPass;
  std::vector<uint8_t> row((475 + 3) / 4);
  std::vector<uint8_t> sourceRow(bitmap.getRowBytes());
  firstPass.reserve(row.size() * 792);
  for (int y = 0; y < 792; y++) {
    ASSERT_EQ(bitmap.readNextRow(row.data(), sourceRow.data()), BmpReaderError::Ok);
    firstPass.insert(firstPass.end(), row.begin(), row.end());
  }
  EXPECT_EQ(fingerprint(firstPass), 819650312U);

  ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
  std::vector<uint8_t> secondPass;
  secondPass.reserve(firstPass.size());
  for (int y = 0; y < 792; y++) {
    ASSERT_EQ(bitmap.readNextRow(row.data(), sourceRow.data()), BmpReaderError::Ok);
    secondPass.insert(secondPass.end(), row.begin(), row.end());
  }
  EXPECT_EQ(secondPass, firstPass);
}

TEST(BitmapResample, ReadsPhysicalRowsBeforeRendererOrientation) {
  HalFile bottomUpFile(create24BitBmp(480, 800));
  HalFile topDownFile(create24BitBmp(480, 800, true));
  Bitmap bottomUp(bottomUpFile, true);
  Bitmap topDown(topDownFile, true);
  ASSERT_EQ(bottomUp.parseHeaders(), BmpReaderError::Ok);
  ASSERT_EQ(topDown.parseHeaders(), BmpReaderError::Ok);
  EXPECT_FALSE(bottomUp.isTopDown());
  EXPECT_TRUE(topDown.isTopDown());
  ASSERT_TRUE(bottomUp.setDitheredOutputSize(475, 792));
  ASSERT_TRUE(topDown.setDitheredOutputSize(475, 792));

  std::vector<uint8_t> bottomUpRow((475 + 3) / 4);
  std::vector<uint8_t> topDownRow((475 + 3) / 4);
  std::vector<uint8_t> bottomUpSourceRow(bottomUp.getRowBytes());
  std::vector<uint8_t> topDownSourceRow(topDown.getRowBytes());
  ASSERT_EQ(bottomUp.readNextRow(bottomUpRow.data(), bottomUpSourceRow.data()), BmpReaderError::Ok);
  ASSERT_EQ(topDown.readNextRow(topDownRow.data(), topDownSourceRow.data()), BmpReaderError::Ok);

  // The same visual gradient is encoded bottom-to-top or top-to-bottom. The
  // decoder must preserve each file's physical order; GfxRenderer uses
  // isTopDown() to place these rows on their matching screen edge.
  EXPECT_EQ(bottomUpRow.front() >> 6, 3U);
  EXPECT_EQ(topDownRow.front() >> 6, 0U);
}

TEST(BitmapResample, ResizingKeepsImageQuantizationSeparateFromTextOverlayLevels) {
  auto bytes = create24BitBmp(4, 4);
  std::fill(bytes.begin() + 54, bytes.end(), 85);
  for (bool imageLevels : {false, true}) {
    HalFile file(bytes);
    Bitmap bitmap(file, true, imageLevels);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    ASSERT_TRUE(bitmap.setDitheredOutputSize(2, 2));
    std::vector<uint8_t> row(1), scratch(bitmap.getRowBytes());
    ASSERT_EQ(bitmap.readNextRow(row.data(), scratch.data()), BmpReaderError::Ok);
    EXPECT_EQ(row[0] >> 6, imageLevels ? 1 : 2);
    ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
    const auto expected = row;
    ASSERT_EQ(bitmap.readNextRow(row.data(), scratch.data()), BmpReaderError::Ok);
    EXPECT_EQ(row, expected);
  }
}

TEST(BitmapResample, PaletteStartsAfterFullInfoHeader) {
  constexpr int kWidth = 64;
  constexpr int kHeight = 32;
  for (const uint32_t infoHeaderSize : {40u, 108u, 124u}) {
    const size_t paletteOffset = 14 + infoHeaderSize;
    const size_t pixelOffset = paletteOffset + 256 * 4;
    for (const uint8_t pixelIndex : {uint8_t{0}, uint8_t{255}}) {
      std::vector<uint8_t> data(pixelOffset + kWidth * kHeight, pixelIndex);
      std::fill(data.begin(), data.begin() + paletteOffset, 0);
      data[0] = 'B';
      data[1] = 'M';
      writeLe32(data, 2, static_cast<uint32_t>(data.size()));
      writeLe32(data, 10, static_cast<uint32_t>(pixelOffset));
      writeLe32(data, 14, infoHeaderSize);
      writeLe32(data, 18, kWidth);
      writeLe32(data, 22, kHeight);
      writeLe16(data, 26, 1);
      writeLe16(data, 28, 8);
      writeLe32(data, 34, kWidth * kHeight);
      writeLe32(data, 46, 256);
      if (infoHeaderSize > 40) {
        // Extended-header fields must not be mistaken for palette entries.
        writeLe32(data, 54, 0x00FF0000);
        writeLe32(data, 58, 0x0000FF00);
        writeLe32(data, 62, 0x000000FF);
        writeLe32(data, 70, 0x73524742);  // 'sRGB'
      }
      for (int i = 0; i < 256; i++) {
        const size_t entry = paletteOffset + static_cast<size_t>(i) * 4;
        data[entry] = data[entry + 1] = data[entry + 2] = static_cast<uint8_t>(i);
        data[entry + 3] = 0;
      }

      for (const bool imageLevels : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "header=" << infoHeaderSize << " index=" << static_cast<int>(pixelIndex)
                                          << " imageLevels=" << imageLevels);
        HalFile file(data);
        Bitmap bitmap(file, true, imageLevels);
        ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
        std::vector<uint8_t> row((kWidth + 3) / 4);
        std::vector<uint8_t> sourceRow(bitmap.getRowBytes());
        const uint8_t expectedByte = pixelIndex == 255 ? 0xFF : 0;
        int unexpectedBytes = 0;
        for (int y = 0; y < kHeight; y++) {
          ASSERT_EQ(bitmap.readNextRow(row.data(), sourceRow.data()), BmpReaderError::Ok);
          for (const uint8_t packed : row) unexpectedBytes += packed != expectedByte;
        }
        EXPECT_EQ(unexpectedBytes, 0);
      }
    }
  }
}

TEST(BitmapResample, RejectsTruncatedExtendedPalette) {
  for (const uint32_t infoHeaderSize : {108u, 124u}) {
    SCOPED_TRACE(infoHeaderSize);
    // The last of the 256 palette entries is missing its red and reserved bytes.
    std::vector<uint8_t> data(14 + infoHeaderSize + 256 * 4 - 2, 0);
    data[0] = 'B';
    data[1] = 'M';
    writeLe32(data, 2, static_cast<uint32_t>(data.size()));
    writeLe32(data, 10, 14 + infoHeaderSize + 256 * 4);
    writeLe32(data, 14, infoHeaderSize);
    writeLe32(data, 18, 1);
    writeLe32(data, 22, 1);
    writeLe16(data, 26, 1);
    writeLe16(data, 28, 8);
    writeLe32(data, 46, 256);

    HalFile file(data);
    Bitmap bitmap(file);
    EXPECT_EQ(bitmap.parseHeaders(), BmpReaderError::FileInvalid);
  }
}

TEST(BitmapResample, RejectsPaletteThatOverlapsPixelData) {
  for (const uint32_t infoHeaderSize : {40u, 108u, 124u}) {
    SCOPED_TRACE(infoHeaderSize);
    // The file has enough bytes for 256 reads, but the last entry is actually pixel data.
    const uint32_t pixelOffset = 14 + infoHeaderSize + 255 * 4;
    std::vector<uint8_t> data(pixelOffset + 4, 0);
    std::fill(data.begin() + pixelOffset, data.end(), 255);
    data[0] = 'B';
    data[1] = 'M';
    writeLe32(data, 2, static_cast<uint32_t>(data.size()));
    writeLe32(data, 10, pixelOffset);
    writeLe32(data, 14, infoHeaderSize);
    writeLe32(data, 18, 4);
    writeLe32(data, 22, 1);
    writeLe16(data, 26, 1);
    writeLe16(data, 28, 8);
    writeLe32(data, 46, 256);

    HalFile file(data);
    Bitmap bitmap(file);
    EXPECT_EQ(bitmap.parseHeaders(), BmpReaderError::FileInvalid);
  }
}

TEST(BitmapResample, Native2BitMapsEveryPackedValueThroughPaletteWithoutDithering) {
  struct PaletteCase {
    std::vector<uint8_t> palette;
    std::vector<uint8_t> levels;
  };
  // These expected levels are explicit for the current adjustment settings
  // (brightness disabled), not calculated with the decoder's pixel helpers.
  const std::vector<PaletteCase> cases{
      {{0, 85, 170, 255}, {0, 1, 2, 3}},
      {{255, 170, 85, 0}, {3, 2, 1, 0}},
      {{255, 0, 170, 85}, {3, 0, 2, 1}},
      {{30, 45, 100, 139}, {0, 0, 1, 2}},
      {{63, 64, 127, 128}, {0, 1, 1, 2}},
      {{191, 192, 254, 255}, {2, 3, 3, 3}},
      {{85, 85, 85, 85}, {1, 1, 1, 1}},
      // Undeclared palette entries retain the parser's default luminances 1, 2, 3.
      {{255}, {3, 0, 0, 0}},
      {{255, 128}, {3, 2, 0, 0}},
      {{255, 128, 64}, {3, 2, 1, 0}},
  };
  std::vector<uint8_t> packed(256);
  for (int i = 0; i < 256; i++) packed[i] = static_cast<uint8_t>(i);

  for (size_t paletteCase = 0; paletteCase < cases.size(); paletteCase++) {
    const auto& testCase = cases[paletteCase];
    // Enumerate the expected four-pixel rows directly from their display levels,
    // independently of the producer's byte/nibble lookup implementation.
    std::vector<uint8_t> expected;
    expected.reserve(258);
    expected.push_back(0xA7);
    for (uint8_t first : testCase.levels) {
      for (uint8_t second : testCase.levels) {
        for (uint8_t third : testCase.levels) {
          for (uint8_t fourth : testCase.levels) {
            expected.push_back(static_cast<uint8_t>((first << 6) | (second << 4) | (third << 2) | fourth));
          }
        }
      }
    }
    expected.push_back(0xA7);
    for (bool dithering : {false, true}) {
      for (bool imageLevels : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "palette=" << paletteCase << " dithering=" << dithering
                                          << " imageLevels=" << imageLevels);
        HalFile file(create2BitBmp(1024, testCase.palette, {packed}));
        Bitmap bitmap(file, dithering, imageLevels);
        ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
        std::vector<uint8_t> row(258, 0xA7), scratch(bitmap.getRowBytes());
        ASSERT_EQ(bitmap.readNextRow(row.data() + 1, scratch.data()), BmpReaderError::Ok);
        EXPECT_EQ(row, expected);
      }
    }
  }
}

TEST(BitmapResample, Native2BitClearsEveryTailWithoutCopyingBmpPadding) {
  struct RowCase {
    int width;
    std::vector<uint8_t> packed;
    std::vector<uint8_t> nativeExpected;
    std::vector<uint8_t> remappedExpected;
  };
  const std::vector<RowCase> cases{
      {1, {0x7F}, {0x40}, {0x00}},
      {2, {0x6F}, {0x60}, {0x20}},
      {3, {0x6F}, {0x6C}, {0x24}},
      {4, {0x6F}, {0x6F}, {0x25}},
      {5, {0x1B, 0xBF}, {0x1B, 0x80}, {0xC9, 0x80}},
      {6, {0x1B, 0xBF}, {0x1B, 0xB0}, {0xC9, 0x90}},
      {7, {0x1B, 0xBF}, {0x1B, 0xBC}, {0xC9, 0x94}},
      {8, {0x1B, 0xBF}, {0x1B, 0xBF}, {0xC9, 0x95}},
      {9, {0x1B, 0xE4, 0xFF}, {0x1B, 0xE4, 0xC0}, {0xC9, 0x63, 0x40}},
      {15, {0x1B, 0xE4, 0x72, 0x9F}, {0x1B, 0xE4, 0x72, 0x9C}, {0xC9, 0x63, 0x1E, 0x84}},
      {16, {0x1B, 0xE4, 0x72, 0x9F}, {0x1B, 0xE4, 0x72, 0x9F}, {0xC9, 0x63, 0x1E, 0x85}},
      {17, {0x1B, 0xE4, 0x72, 0x9F, 0x7F}, {0x1B, 0xE4, 0x72, 0x9F, 0x40}, {0xC9, 0x63, 0x1E, 0x85, 0x00}},
  };
  for (const auto& testCase : cases) {
    for (bool remap : {false, true}) {
      for (bool defaultColorsUsed : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "width=" << testCase.width << " remap=" << remap
                                          << " defaultColorsUsed=" << defaultColorsUsed);
        const std::vector<uint8_t> palette = remap ? std::vector<uint8_t>{255, 0, 170, 85}
                                                  : std::vector<uint8_t>{0, 85, 170, 255};
        HalFile file(create2BitBmp(testCase.width, palette, {testCase.packed}, false, defaultColorsUsed));
        Bitmap bitmap(file, true, true);
        ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
        const auto& pixels = remap ? testCase.remappedExpected : testCase.nativeExpected;
        std::vector<uint8_t> expected(pixels.size() + 2, 0xA7), row(expected);
        std::copy(pixels.begin(), pixels.end(), expected.begin() + 1);
        std::vector<uint8_t> scratch(bitmap.getRowBytes());
        ASSERT_EQ(bitmap.readNextRow(row.data() + 1, scratch.data()), BmpReaderError::Ok);
        EXPECT_EQ(row, expected);
      }
    }
  }
}

TEST(BitmapResample, Native2BitPreservesPhysicalOrderEndOfImageAndRewind) {
  const std::vector<std::vector<uint8_t>> packed{{0x1B, 0x6F}, {0xE4, 0xBF}, {0x72, 0xCF}};
  for (bool topDown : {false, true}) {
    for (bool remap : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "topDown=" << topDown << " remap=" << remap);
      const std::vector<uint8_t> palette =
          remap ? std::vector<uint8_t>{255, 0, 170, 85} : std::vector<uint8_t>{0, 85, 170, 255};
      const std::vector<std::vector<uint8_t>> expected =
          remap ? std::vector<std::vector<uint8_t>>{{0xC9, 0x24}, {0x63, 0x94}, {0x1E, 0x74}}
                : std::vector<std::vector<uint8_t>>{{0x1B, 0x6C}, {0xE4, 0xBC}, {0x72, 0xCC}};
      auto bytes = create2BitBmp(7, palette, packed, topDown);
      bytes.insert(bytes.end(), {0xDE, 0xAD, 0xBE, 0xEF});
      writeLe32(bytes, 2, static_cast<uint32_t>(bytes.size()));
      HalFile file(bytes);
      Bitmap bitmap(file, true, true);
      ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
      EXPECT_EQ(bitmap.isTopDown(), topDown);
      std::vector<uint8_t> row(4, 0xA7), scratch(bitmap.getRowBytes());
      for (int pass = 0; pass < 2; pass++) {
        if (pass) ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
        for (int y = 0; y < 3; y++) {
          const int visualRow = topDown ? y : 2 - y;
          ASSERT_EQ(bitmap.readNextRow(row.data() + 1, scratch.data()), BmpReaderError::Ok);
          EXPECT_EQ(row, (std::vector<uint8_t>{0xA7, expected[visualRow][0], expected[visualRow][1], 0xA7}));
          EXPECT_EQ(scratch, (std::vector<uint8_t>{packed[visualRow][0], packed[visualRow][1], 0xA5, 0xA5}));
        }
        const auto lastRow = row;
        EXPECT_EQ(bitmap.readNextRow(row.data() + 1, scratch.data()), BmpReaderError::ShortReadRow);
        EXPECT_EQ(bitmap.readNextRow(row.data() + 1, scratch.data()), BmpReaderError::ShortReadRow);
        EXPECT_EQ(row, lastRow);
        // End-of-image is a row-count limit, not a read of the trailing bytes.
        EXPECT_EQ(file.read(), 0xDE);
      }
    }
  }
}

TEST(BitmapResample, Native2BitRejectsShortRowsBeforeChangingOutput) {
  for (bool remap : {false, true}) {
    for (int completeRows : {0, 1}) {
      for (int partialBytes = 0; partialBytes < 4; partialBytes++) {
        SCOPED_TRACE(::testing::Message() << "remap=" << remap << " completeRows=" << completeRows
                                          << " partialBytes=" << partialBytes);
        const std::vector<uint8_t> palette = remap ? std::vector<uint8_t>{255, 0, 170, 85}
                                                  : std::vector<uint8_t>{0, 85, 170, 255};
        const std::vector<std::vector<uint8_t>> packed =
            completeRows ? std::vector<std::vector<uint8_t>>{{0x1B, 0xBF}, {0xE4, 0x7F}}
                         : std::vector<std::vector<uint8_t>>{{0xE4, 0x7F}};
        auto bytes = create2BitBmp(5, palette, packed, true);
        // A complete packed pixel payload is still short if BMP row padding is missing.
        bytes.resize(bytes.size() - (4 - partialBytes));
        HalFile file(bytes);
        Bitmap bitmap(file, true, true);
        ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
        std::vector<uint8_t> row(4, 0xA7), scratch(bitmap.getRowBytes());
        for (int pass = 0; pass < 2; pass++) {
          if (pass) ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
          if (completeRows) {
            ASSERT_EQ(bitmap.readNextRow(row.data() + 1, scratch.data()), BmpReaderError::Ok);
            EXPECT_EQ(row, (std::vector<uint8_t>{0xA7, static_cast<uint8_t>(remap ? 0xC9 : 0x1B), 0x80, 0xA7}));
          }
          std::fill(row.begin(), row.end(), 0xA7);
          EXPECT_EQ(bitmap.readNextRow(row.data() + 1, scratch.data()), BmpReaderError::ShortReadRow);
          EXPECT_EQ(row, (std::vector<uint8_t>{0xA7, 0xA7, 0xA7, 0xA7}));
          EXPECT_EQ(bitmap.readNextRow(row.data() + 1, scratch.data()), BmpReaderError::ShortReadRow);
          EXPECT_EQ(row, (std::vector<uint8_t>{0xA7, 0xA7, 0xA7, 0xA7}));
        }
      }
    }
  }
}

TEST(BitmapResample, Native2BitCanNormalizeInExistingRowStorage) {
  for (bool remap : {false, true}) {
    const std::vector<uint8_t> palette =
        remap ? std::vector<uint8_t>{255, 0, 170, 85} : std::vector<uint8_t>{0, 85, 170, 255};
    HalFile file(create2BitBmp(7, palette, {{0x1B, 0xBF}}));
    Bitmap bitmap(file);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    std::vector<uint8_t> row(bitmap.getRowBytes(), 0);
    ASSERT_EQ(bitmap.readNextRow(row.data(), row.data()), BmpReaderError::Ok);
    EXPECT_EQ(row, (remap ? std::vector<uint8_t>{0xC9, 0x94, 0xA5, 0xA5}
                          : std::vector<uint8_t>{0x1B, 0xBC, 0xA5, 0xA5}));
  }
}
