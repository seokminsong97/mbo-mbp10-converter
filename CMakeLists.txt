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
set(
  MBO_MBP10_ARROW_ROOT
  ""
  CACHE PATH
  "Arrow C++ prefix (or PyArrow package directory) when CMake packages are unavailable"
)

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

find_package(Arrow CONFIG QUIET)
find_package(Parquet CONFIG QUIET)

set(MBO_MBP10_ARROW_TARGET "")
foreach(candidate Arrow::arrow_shared Arrow::arrow_static Arrow::arrow)
  if(TARGET ${candidate})
    set(MBO_MBP10_ARROW_TARGET ${candidate})
    break()
  endif()
endforeach()

set(MBO_MBP10_PARQUET_TARGET "")
foreach(candidate Parquet::parquet_shared Parquet::parquet_static
                  Parquet::parquet)
  if(TARGET ${candidate})
    set(MBO_MBP10_PARQUET_TARGET ${candidate})
    break()
  endif()
endforeach()

# PyArrow wheels contain the complete Arrow/Parquet C++ headers and shared
# libraries but do not ship CMake package files. This explicit prefix fallback
# also supports other self-contained Arrow distributions.
if((NOT MBO_MBP10_ARROW_TARGET OR NOT MBO_MBP10_PARQUET_TARGET)
   AND MBO_MBP10_ARROW_ROOT)
  find_path(
    MBO_MBP10_ARROW_INCLUDE_DIR
    arrow/api.h
    PATHS
      "${MBO_MBP10_ARROW_ROOT}/include"
    NO_DEFAULT_PATH
  )
  if(APPLE)
    file(
      GLOB MBO_MBP10_ARROW_LIBRARIES
      "${MBO_MBP10_ARROW_ROOT}/libarrow.dylib"
      "${MBO_MBP10_ARROW_ROOT}/libarrow.*.dylib"
      "${MBO_MBP10_ARROW_ROOT}/lib/libarrow.dylib"
      "${MBO_MBP10_ARROW_ROOT}/lib/libarrow.*.dylib"
    )
    file(
      GLOB MBO_MBP10_PARQUET_LIBRARIES
      "${MBO_MBP10_ARROW_ROOT}/libparquet.dylib"
      "${MBO_MBP10_ARROW_ROOT}/libparquet.*.dylib"
      "${MBO_MBP10_ARROW_ROOT}/lib/libparquet.dylib"
      "${MBO_MBP10_ARROW_ROOT}/lib/libparquet.*.dylib"
    )
  elseif(UNIX)
    file(
      GLOB MBO_MBP10_ARROW_LIBRARIES
      "${MBO_MBP10_ARROW_ROOT}/libarrow.so"
      "${MBO_MBP10_ARROW_ROOT}/libarrow.so.*"
      "${MBO_MBP10_ARROW_ROOT}/lib/libarrow.so"
      "${MBO_MBP10_ARROW_ROOT}/lib/libarrow.so.*"
    )
    file(
      GLOB MBO_MBP10_PARQUET_LIBRARIES
      "${MBO_MBP10_ARROW_ROOT}/libparquet.so"
      "${MBO_MBP10_ARROW_ROOT}/libparquet.so.*"
      "${MBO_MBP10_ARROW_ROOT}/lib/libparquet.so"
      "${MBO_MBP10_ARROW_ROOT}/lib/libparquet.so.*"
    )
  endif()

  list(SORT MBO_MBP10_ARROW_LIBRARIES)
  list(SORT MBO_MBP10_PARQUET_LIBRARIES)
  if(MBO_MBP10_ARROW_LIBRARIES)
    list(GET MBO_MBP10_ARROW_LIBRARIES 0 MBO_MBP10_ARROW_LIBRARY)
  endif()
  if(MBO_MBP10_PARQUET_LIBRARIES)
    list(GET MBO_MBP10_PARQUET_LIBRARIES 0 MBO_MBP10_PARQUET_LIBRARY)
  endif()

  if(MBO_MBP10_ARROW_INCLUDE_DIR AND MBO_MBP10_ARROW_LIBRARY
     AND MBO_MBP10_PARQUET_LIBRARY)
    add_library(mbo_mbp10_arrow SHARED IMPORTED)
    set_target_properties(
      mbo_mbp10_arrow
      PROPERTIES
        IMPORTED_LOCATION "${MBO_MBP10_ARROW_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MBO_MBP10_ARROW_INCLUDE_DIR}"
    )
    add_library(mbo_mbp10_parquet SHARED IMPORTED)
    set_target_properties(
      mbo_mbp10_parquet
      PROPERTIES
        IMPORTED_LOCATION "${MBO_MBP10_PARQUET_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MBO_MBP10_ARROW_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES mbo_mbp10_arrow
    )
    set(MBO_MBP10_ARROW_TARGET mbo_mbp10_arrow)
    set(MBO_MBP10_PARQUET_TARGET mbo_mbp10_parquet)
  endif()
endif()

if(NOT MBO_MBP10_ARROW_TARGET OR NOT MBO_MBP10_PARQUET_TARGET)
  message(
    FATAL_ERROR
    "Apache Arrow and Parquet C++ are required. Install them or set "
    "MBO_MBP10_ARROW_ROOT to an Arrow prefix or PyArrow package directory."
  )
endif()

add_library(
  mbo_mbp10_converter
  STATIC
    src/converter.cpp
    src/file_converter.cpp
    src/input_validator.cpp
    src/parquet_writer.cpp
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
target_link_libraries(
  mbo_mbp10_converter
  PUBLIC databento::databento
  PRIVATE
    ${MBO_MBP10_ARROW_TARGET}
    ${MBO_MBP10_PARQUET_TARGET}
)

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
  target_link_libraries(
    mbo_mbp10_tests
    PRIVATE
      mbo_mbp10::converter
      ${MBO_MBP10_ARROW_TARGET}
      ${MBO_MBP10_PARQUET_TARGET}
  )
  add_test(NAME mbo_mbp10_tests COMMAND mbo_mbp10_tests)
endif()
