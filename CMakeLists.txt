cmake_minimum_required(VERSION 3.20)
project(StreamVault VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(streamvault_core
  src/common/serialization.cpp
  src/common/statistics.cpp
  src/common/session.cpp
  src/common/fault_injection.cpp
  src/common/producer.cpp)
target_include_directories(streamvault_core PUBLIC include)
target_link_libraries(streamvault_core PUBLIC Threads::Threads)
target_compile_options(streamvault_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(imu_producer src/producers/imu_main.cpp)
add_executable(gps_producer src/producers/gps_main.cpp)
add_executable(temperature_producer src/producers/temperature_main.cpp)
add_executable(streamvault_recorder src/recorder/main.cpp)
add_executable(streamvault_replay src/replay/main.cpp)
foreach(target imu_producer gps_producer temperature_producer streamvault_recorder streamvault_replay)
  target_link_libraries(${target} PRIVATE streamvault_core)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

include(CTest)
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  include(GoogleTest)
  add_executable(streamvault_tests tests/streamvault_tests.cpp)
  target_link_libraries(streamvault_tests PRIVATE streamvault_core GTest::gtest_main)
  target_compile_options(streamvault_tests PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(streamvault_tests)
endif()
