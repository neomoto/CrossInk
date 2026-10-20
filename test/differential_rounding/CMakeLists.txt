add_executable(DifferentialRoundingTest
  DifferentialRoundingTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(DifferentialRoundingTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(DifferentialRoundingTest PRIVATE CROSSINK_SCALABLE_FONTS=1)

target_link_libraries(DifferentialRoundingTest PRIVATE crosspoint_test_common)
target_compile_options(DifferentialRoundingTest PRIVATE -UNDEBUG)

add_test(NAME DifferentialRoundingTest COMMAND DifferentialRoundingTest)

add_executable(KerningLookupTest
  KerningLookupTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(KerningLookupTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)
target_compile_definitions(KerningLookupTest PRIVATE CROSSINK_SCALABLE_FONTS=1)
target_link_libraries(KerningLookupTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(KerningLookupTest)
