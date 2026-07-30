cmake_minimum_required(VERSION 3.28)
project(informix_odbc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(IFXODBC_BUILD_TESTS "Build unit/integration tests" ON)

add_subdirectory(sqli)
add_subdirectory(driver)
add_subdirectory(tools/ifx_probe)
add_subdirectory(tools/odbc_dm_probe)

if(IFXODBC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
