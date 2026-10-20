add_executable(EpubGrayscaleTest EpubGrayscaleTest.cpp
  ${REPO_ROOT}/src/activities/reader/EpubGrayscale.cpp)
target_include_directories(EpubGrayscaleTest PRIVATE stubs ../memory_policy/stubs
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget
  ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/src/activities/reader)
target_compile_definitions(EpubGrayscaleTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(EpubGrayscaleTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(EpubGrayscaleTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

# Real text/glyph rasterization with a memory-only display and storage backend.
set(EPUB_TEXT_GRAYSCALE_SOURCES EpubTextGrayscaleTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
function(add_epub_text_grayscale_target target)
  add_executable(${target} ${EPUB_TEXT_GRAYSCALE_SOURCES})
  target_include_directories(${target} PRIVATE raster_stubs
    ${REPO_ROOT}/include ${REPO_ROOT}/src/activities/reader
    ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include
    ${REPO_ROOT}/lib/GfxRenderer ../memory_policy/stubs
    ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Memory
    ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/MiniBidi
    ${REPO_ROOT}/lib/miniz/src ${REPO_ROOT}/lib/InflateReader ${REPO_ROOT}/lib/uzlib/src)
  target_compile_definitions(${target} PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1
    CROSSINK_APP_CAP_TOUCH=0 CROSSINK_APP_CAP_USB_DRIVE=0)
  target_link_libraries(${target} PRIVATE crosspoint_test_common GTest::gtest_main)
endfunction()

add_epub_text_grayscale_target(EpubTextGrayscaleTest)
target_compile_definitions(EpubTextGrayscaleTest PRIVATE CROSSINK_SCALABLE_FONTS=0)
gtest_discover_tests(EpubTextGrayscaleTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

# The same independent raster oracle also covers callback-owned glyph lifetimes.
# Link the production scalable adapter/SDK, not substitutes for its behavior.
set(EPUB_SCALABLE_FONT_SDK ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont)
file(GLOB EPUB_SCALABLE_FONT_SOURCES
  ${EPUB_SCALABLE_FONT_SDK}/src/*.c
  ${EPUB_SCALABLE_FONT_SDK}/src/*.cpp
  ${EPUB_SCALABLE_FONT_SDK}/src/freetype/*.c)
add_epub_text_grayscale_target(EpubTextScalableGrayscaleTest)
target_sources(EpubTextScalableGrayscaleTest PRIVATE
  ${REPO_ROOT}/lib/ScalableFont/HalScalableFont.cpp ${EPUB_SCALABLE_FONT_SOURCES})
target_include_directories(EpubTextScalableGrayscaleTest PRIVATE
  ${REPO_ROOT}/lib/ScalableFont ${EPUB_SCALABLE_FONT_SDK}/include
  ${EPUB_SCALABLE_FONT_SDK}/third_party/freetype/include ${EPUB_SCALABLE_FONT_SDK}/third_party/stb)
# Keep the bitmap target's fake-ESP allocation/deallocation pairing; SIMULATOR
# would bypass its heap headers/deleter while CROSSINK_MEMORY_TEST still allocates there.
target_compile_definitions(EpubTextScalableGrayscaleTest PRIVATE CROSSINK_SCALABLE_FONTS=1
  FREEINK_FONT_ENABLE_AUTOHINT=1 FREEINK_FONT_ENABLE_NATIVE_HINTING=1 FREEINK_FONT_ENABLE_MONOCHROME=1)
gtest_discover_tests(EpubTextScalableGrayscaleTest TEST_PREFIX "ScalableRaster."
  PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

add_executable(RetainedPxcCacheTest RetainedPxcCacheTest.cpp
  ${REPO_ROOT}/src/activities/reader/EpubGrayscale.cpp)
target_include_directories(RetainedPxcCacheTest PRIVATE stubs ../memory_policy/stubs
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget
  ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/src/activities/reader)
target_compile_definitions(RetainedPxcCacheTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(RetainedPxcCacheTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(RetainedPxcCacheTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
