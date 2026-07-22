cmake_minimum_required(VERSION 3.24)

project(rudp_rt_gateway
  VERSION 1.0.0
  DESCRIPTION "RUDP-RT deterministic deadline-aware reliable media transport over UDP/RTP"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(RUDP_WERROR "Treat compiler warnings as errors" ON)
option(RUDP_BUILD_TESTS "Build test targets" ON)

set(RUDP_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(RUDP_WERROR)
  list(APPEND RUDP_WARNING_FLAGS -Werror)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core protocol library
# ---------------------------------------------------------------------------
add_library(rudp_rt STATIC
  src/codec/rudp_codec.cpp
  src/codec/rtp_codec.cpp
  src/transport/sender.cpp
  src/transport/receiver.cpp
  src/transport/frame_assembler.cpp
  src/transport/jitter_buffer.cpp
  src/transport/playout_pipeline.cpp
  src/session/session.cpp
  src/session/session_manager.cpp
  src/runtime/config.cpp
  src/runtime/event_loop.cpp
  src/runtime/reactor.cpp
  src/runtime/timer_heap.cpp
  src/runtime/socket_network.cpp
  src/runtime/gateway.cpp
  src/runtime/client.cpp
  src/media/synthetic_source.cpp
  src/media/camera_source.cpp
  src/media/packetizer.cpp
  src/metrics/metrics.cpp
  src/metrics/report.cpp
)

target_include_directories(rudp_rt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(rudp_rt PRIVATE ${RUDP_WARNING_FLAGS})
target_link_libraries(rudp_rt PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
foreach(app rudp_gateway rudp_client rudp_sim rudp_bench)
  string(REGEX REPLACE "^rudp_" "" src_name "${app}")
  add_executable(${app} apps/${src_name}_main.cpp)
  target_compile_options(${app} PRIVATE ${RUDP_WARNING_FLAGS})
  target_link_libraries(${app} PRIVATE rudp_rt)
endforeach()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(RUDP_BUILD_TESTS)
  enable_testing()

  add_subdirectory(tests/unit)
  add_subdirectory(tests/property)
  add_subdirectory(tests/integration)
  add_subdirectory(tests/fault)
  add_subdirectory(tests/system)
endif()

# ---------------------------------------------------------------------------
# Sanitizer variants are produced by passing:
#   -DRUDP_SANITIZE=address   (ASan + UBSan)
#   -DRUDP_SANITIZE=thread    (TSan)
# ---------------------------------------------------------------------------
set(RUDP_SANITIZE "" CACHE STRING "Sanitizer: address|thread|undefined")
if(RUDP_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
elseif(RUDP_SANITIZE STREQUAL "undefined")
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=undefined)
elseif(RUDP_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()
