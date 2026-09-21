cmake_minimum_required(VERSION 3.16)
project(myalloc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # -std=c++17, not -std=gnu++17, so -Wpedantic means something

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MYALLOC_BUILD_TESTS  "Build the test suite" ON)
option(MYALLOC_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(MYALLOC_WERROR       "Treat warnings as errors" OFF)

set(WARNINGS -Wall -Wextra -Wpedantic)
if(MYALLOC_WERROR)
  list(APPEND WARNINGS -Werror)
endif()

# UBSan has to be applied to everything at once, so it goes on globally.
#
# There is deliberately no AddressSanitizer option: ASan replaces malloc and
# cannot see inside a custom allocator, so it would find nothing here. The heap
# checker does that job instead. See README.
if(MYALLOC_ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all -g)
  add_link_options(-fsanitize=undefined)
endif()

# --- the library -------------------------------------------------------------
add_library(myalloc STATIC
  src/allocator.cpp
  src/heapcheck.cpp
)
target_include_directories(myalloc
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(myalloc PRIVATE ${WARNINGS})

find_package(Threads REQUIRED)
target_link_libraries(myalloc PUBLIC Threads::Threads)

# --- the driver --------------------------------------------------------------
add_executable(myalloc_cli cli/main.cpp)
target_link_libraries(myalloc_cli PRIVATE myalloc)
target_compile_options(myalloc_cli PRIVATE ${WARNINGS})

# --- tests -------------------------------------------------------------------
if(MYALLOC_BUILD_TESTS)
  enable_testing()

  # Use the system GoogleTest if it's installed, otherwise fetch it.
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(myalloc_tests tests/test_allocator.cpp)
  target_include_directories(myalloc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(myalloc_tests PRIVATE myalloc GTest::gtest GTest::gtest_main)
  target_compile_options(myalloc_tests PRIVATE ${WARNINGS})

  include(GoogleTest)
  gtest_discover_tests(myalloc_tests DISCOVERY_MODE PRE_TEST)
endif()
