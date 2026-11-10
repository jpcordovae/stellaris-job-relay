cmake_minimum_required(VERSION 3.25)
project(stellaris_job_relay VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(JRELAY_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(JRELAY_BUILD_TESTS "Build unit and end-to-end tests" ON)

# --- Dependencies -----------------------------------------------------------

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq>=4.3)
find_path(CPPZMQ_INCLUDE_DIR zmq.hpp REQUIRED)

include(FetchContent)
FetchContent_Declare(
  blake3
  GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3.git
  GIT_TAG        1.8.7
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  c
)
FetchContent_MakeAvailable(blake3)

# --- Common compile flags ---------------------------------------------------

add_library(jrelay_options INTERFACE)
target_compile_options(jrelay_options INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor
  -Wold-style-cast -Wnull-dereference -Wimplicit-fallthrough)
if(JRELAY_SANITIZE)
  target_compile_options(jrelay_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(jrelay_options INTERFACE -fsanitize=address,undefined)
endif()

# --- Core library (protocol, server, client) --------------------------------

add_library(jrelay_core STATIC
  src/protocol/wire.cpp
  src/protocol/message.cpp
  src/common/digest.cpp
  src/common/ids.cpp
  src/server/memory.cpp
  src/server/store.cpp
  src/server/auth.cpp
  src/server/server.cpp
  src/client/client.cpp
)
target_include_directories(jrelay_core PUBLIC src ${CPPZMQ_INCLUDE_DIR})
target_link_libraries(jrelay_core PUBLIC PkgConfig::ZMQ BLAKE3::blake3 jrelay_options)

add_executable(jrelay-server src/server/main.cpp)
target_link_libraries(jrelay-server PRIVATE jrelay_core)

add_executable(jrelay src/client/main.cpp)
target_link_libraries(jrelay PRIVATE jrelay_core)

# --- Tests ------------------------------------------------------------------

if(JRELAY_BUILD_TESTS)
  enable_testing()
  find_path(DOCTEST_INCLUDE_DIR doctest/doctest.h REQUIRED)
  add_executable(jrelay_tests
    tests/main.cpp
    tests/test_wire.cpp
    tests/test_store.cpp
    tests/test_end_to_end.cpp
  )
  target_include_directories(jrelay_tests PRIVATE ${DOCTEST_INCLUDE_DIR})
  target_link_libraries(jrelay_tests PRIVATE jrelay_core)
  add_test(NAME jrelay_tests COMMAND jrelay_tests)
endif()
