#include <Epub/blocks/ImageBlock.h>
#include <gtest/gtest.h>

// These tests exercise production Page/TextBlock persistence, not image I/O.
// Fail explicitly if an image path is reached instead of supplying fake bytes.
void ImageBlock::render(GfxRenderer&, int, int, bool) { ADD_FAILURE() << "Unexpected image render"; }
void ImageBlock::renderPlaceholder(GfxRenderer&, int, int, bool) const {
  ADD_FAILURE() << "Unexpected image placeholder";
}
void ImageBlock::prepareCache() const { ADD_FAILURE() << "Unexpected image cache preparation"; }
bool ImageBlock::needsDecode() const {
  ADD_FAILURE() << "Unexpected image decode check";
  return false;
}
bool ImageBlock::serialize(FsFile&) {
  ADD_FAILURE() << "Unexpected image serialization";
  return false;
}
#ifdef PSRAM_REFERENCE
std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile&) {
  ADD_FAILURE() << "Unexpected image deserialization";
  return nullptr;
}
#else
std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile&, PageReadBudget*) {
  ADD_FAILURE() << "Unexpected image deserialization";
  return nullptr;
}
#endif
