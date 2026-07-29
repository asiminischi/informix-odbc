add_executable(sqli_tests
  test_version.cpp
  test_handshake_encode.cpp
)
target_link_libraries(sqli_tests PRIVATE ifx_sqli doctest::doctest)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(sqli_tests)
