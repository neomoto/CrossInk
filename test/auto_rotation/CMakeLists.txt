add_executable(AutoRotationTest
  AutoRotationTest.cpp
  HalTiltSensorTest.cpp
  ${REPO_ROOT}/lib/hal/HalTiltSensor.cpp
)

target_include_directories(AutoRotationTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(AutoRotationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AutoRotationTest)
