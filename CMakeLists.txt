cmake_minimum_required(VERSION 3.20)
project(quarry CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  # Benchmarks of a query engine are meaningless at -O0, and an unoptimized build
  # is the most common way to accidentally report a vectorization result that is
  # really just the optimizer being switched off.
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# -DNDEBUG matters: without it, assert() survives into this build, and
# Batch::SetSelection asserts std::is_sorted over the whole selection vector on
# every filter. Measured at no detectable cost on the current kernel, but a
# release build that silently runs O(n) assertions per batch is a trap waiting
# for whoever optimizes the kernel.
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

option(QUARRY_ASAN "Build with address and undefined-behaviour sanitizers" OFF)
if(QUARRY_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# The engine
# ---------------------------------------------------------------------------
file(GLOB_RECURSE QUARRY_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(quarry STATIC ${QUARRY_SOURCES})
target_include_directories(quarry PUBLIC include)

# ---------------------------------------------------------------------------
# Shell
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/quarry_shell.cpp)
  add_executable(quarry_shell tools/quarry_shell.cpp)
  target_link_libraries(quarry_shell PRIVATE quarry)
endif()

# ---------------------------------------------------------------------------
# Tests. One executable per file keeps a crash in one suite from hiding the rest.
# ---------------------------------------------------------------------------
enable_testing()

file(GLOB QUARRY_TEST_SOURCES CONFIGURE_DEPENDS tests/*_test.cpp)
foreach(test_src ${QUARRY_TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE quarry)
  target_include_directories(${test_name} PRIVATE tests)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# The differential suite compares Quarry against SQLite, so that one test target
# links libsqlite3. Nothing else in the project depends on it.
if(TARGET differential_test)
  find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
  find_library(SQLITE3_LIBRARY sqlite3)
  if(SQLITE3_INCLUDE_DIR AND SQLITE3_LIBRARY)
    target_include_directories(differential_test PRIVATE ${SQLITE3_INCLUDE_DIR})
    target_link_libraries(differential_test PRIVATE ${SQLITE3_LIBRARY})
    target_compile_definitions(differential_test PRIVATE QUARRY_HAVE_SQLITE3)
  else()
    message(WARNING "sqlite3 not found: differential_test will fail rather than silently skip")
  endif()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
file(GLOB QUARRY_BENCH_SOURCES CONFIGURE_DEPENDS bench/*_bench.cpp)
foreach(bench_src ${QUARRY_BENCH_SOURCES})
  get_filename_component(bench_name ${bench_src} NAME_WE)
  add_executable(${bench_name} ${bench_src})
  target_link_libraries(${bench_name} PRIVATE quarry)
  target_include_directories(${bench_name} PRIVATE tests bench)
endforeach()
