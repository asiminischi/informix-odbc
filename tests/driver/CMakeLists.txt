# Unit tests for driver/-layer logic that's pure (no live server, no ODBC
# Driver Manager) and worth a durable regression guard - currently just
# bind_encode.cpp's INTERVAL qualifier reconstruction, the one piece of new
# logic in that file that doesn't reduce to an already-live-verified sqli-
# layer encoder. Compiles driver/bind_encode.cpp directly into the test
# binary (it has no dependency on driver/handles.h or the ifxodbc DLL
# target) rather than linking the built ifxodbc library, avoiding
# DLL-export visibility entirely for something this self-contained.
add_executable(driver_tests
  test_bind_encode.cpp
  ${CMAKE_SOURCE_DIR}/driver/bind_encode.cpp
)
target_include_directories(driver_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(driver_tests PRIVATE ifx_sqli doctest::doctest)
target_compile_features(driver_tests PUBLIC cxx_std_26)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(driver_tests)
