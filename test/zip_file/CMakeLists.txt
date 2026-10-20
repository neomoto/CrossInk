find_package(ZLIB REQUIRED)

add_executable(ZipFileLookupTest
  ZipFileLookupTest.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
)
target_include_directories(ZipFileLookupTest PRIVATE
  stubs
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/miniz/src
  ${REPO_ROOT}/test/pxc_v2/stubs
  ${REPO_ROOT}/lib/Memory
)
target_link_libraries(ZipFileLookupTest PRIVATE crosspoint_test_common GTest::gtest_main ZLIB::ZLIB)
gtest_discover_tests(ZipFileLookupTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
