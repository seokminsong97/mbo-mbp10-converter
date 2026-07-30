cmake_minimum_required(VERSION 3.24)

project(
  mbo_mbp10_converter
  VERSION 0.1.0
  DESCRIPTION "Unofficial offline Databento MBO to MBP-10 converter"
  LANGUAGES CXX
)

option(MBO_MBP10_USE_SYSTEM_DATABENTO
       "Use an installed databento-cpp package instead of FetchContent" OFF)
option(MBO_MBP10_BUILD_TESTS "Build converter tests" ON)

if(MBO_MBP10_USE_SYSTEM_DATABENTO)
  find_package(databento 0.62 REQUIRED)
else()
  include(FetchContent)

  # Pin the official SDK. Its DBN reader upgrades supported older DBN versions
  # to the current in-memory record layout.
  set(DATABENTO_ENABLE_UNIT_TESTING OFF CACHE BOOL "" FORCE)
  set(DATABENTO_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(DATABENTO_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  set(DATABENTO_VERBOSE_OUTPUT OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    databento
    GIT_REPOSITORY https://github.com/databento/databento-cpp.git
    GIT_TAG v0.62.1
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(databento)
endif()

add_library(
  mbo_mbp10_converter
  STATIC
    src/converter.cpp
    src/file_converter.cpp
    src/input_validator.cpp
)
add_library(mbo_mbp10::converter ALIAS mbo_mbp10_converter)

target_compile_features(mbo_mbp10_converter PUBLIC cxx_std_17)
set_target_properties(
  mbo_mbp10_converter
  PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(
  mbo_mbp10_converter
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(mbo_mbp10_converter PUBLIC databento::databento)

if(MSVC)
  target_compile_options(mbo_mbp10_converter PRIVATE /W4 /permissive-)
else()
  target_compile_options(
    mbo_mbp10_converter
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

add_executable(mbo-mbp10 src/main.cpp)
target_compile_features(mbo-mbp10 PRIVATE cxx_std_17)
set_target_properties(mbo-mbp10 PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(
  mbo-mbp10
  PRIVATE MBO_MBP10_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(mbo-mbp10 PRIVATE mbo_mbp10::converter)

if(MBO_MBP10_BUILD_TESTS)
  enable_testing()
  add_executable(
    mbo_mbp10_tests
    tests/converter_tests.cpp
  )
  target_compile_features(mbo_mbp10_tests PRIVATE cxx_std_17)
  set_target_properties(mbo_mbp10_tests PROPERTIES CXX_EXTENSIONS OFF)
  target_link_libraries(mbo_mbp10_tests PRIVATE mbo_mbp10::converter)
  add_test(NAME mbo_mbp10_tests COMMAND mbo_mbp10_tests)
endif()
