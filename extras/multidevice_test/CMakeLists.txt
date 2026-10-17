# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host unit tests of the multidevice session (src/Z2S_multidevice) against an
# in-memory Supla server.
#
#   cmake -S extras/multidevice_test -B build/multidevice_test
#   cmake --build build/multidevice_test -j
#   ctest --test-dir build/multidevice_test
#
# SUPLA_DEVICE_DIR points to the supla-device library used to build Z2S
# (for the SRPC protocol code in src/supla-common).

cmake_minimum_required(VERSION 3.16)
project(z2s_multidevice_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(Z2S_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SUPLA_DEVICE_DIR
    ${Z2S_ROOT}/.pio/libdeps/esp32c6_8MB_dyn/SuplaDevice
    CACHE PATH "supla-device library directory")
if(NOT EXISTS ${SUPLA_DEVICE_DIR}/src/supla-common/srpc.c)
  message(FATAL_ERROR "supla-device not found in ${SUPLA_DEVICE_DIR}")
endif()

option(Z2S_SANITIZE "Build with AddressSanitizer and UBSan" ON)
if(Z2S_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
    -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)

# SUPLA_DEVICE selects the device side, single threaded build of srpc.
add_library(supla_common STATIC
  ${SUPLA_DEVICE_DIR}/src/supla-common/srpc.c
  ${SUPLA_DEVICE_DIR}/src/supla-common/proto.c
  ${SUPLA_DEVICE_DIR}/src/supla-common/lck.c
  ${SUPLA_DEVICE_DIR}/src/supla-common/log.c
)
target_include_directories(supla_common PUBLIC ${SUPLA_DEVICE_DIR}/src)
target_compile_definitions(supla_common PUBLIC SUPLA_DEVICE)
target_compile_options(supla_common PRIVATE -w)

add_library(z2s_multidevice STATIC
  ${Z2S_ROOT}/src/Z2S_multidevice/Z2S_MultiDeviceSession.cpp
)
target_include_directories(z2s_multidevice PUBLIC
  ${Z2S_ROOT}/src/Z2S_multidevice)
target_link_libraries(z2s_multidevice PUBLIC supla_common)
target_compile_options(z2s_multidevice PRIVATE -Wall -Wextra
  -Wno-unused-parameter)

add_executable(z2s_multidevice_tests
  fake_supla_server.cpp
  session_test.cpp
  supla_log.cpp
)
target_link_libraries(z2s_multidevice_tests PRIVATE
  z2s_multidevice GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(z2s_multidevice_tests)
