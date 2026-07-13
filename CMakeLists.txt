cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME url_tools)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vendored code lives in third_party/ so the format/tidy gates (scoped to src/
# and test/) never touch it.
include_directories(src/include third_party third_party/ada third_party/psl)

set(EXTENSION_SOURCES third_party/ada/ada.cpp third_party/psl/psl_rules.cpp
                      src/url_tools_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# ada-url requires C++20.
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_20)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_20)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
