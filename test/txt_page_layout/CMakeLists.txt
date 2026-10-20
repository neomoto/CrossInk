add_executable(TxtPageLayoutTest
  TxtPageLayoutTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TxtPageLayoutTest PRIVATE
  ${REPO_ROOT}/test/section_persistence/stubs
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Txt
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(TxtPageLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TxtPageLayoutTest)
