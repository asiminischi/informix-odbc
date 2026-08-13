include(FetchContent)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
)
# doctest 2.4.11's own CMakeLists predates CMake's minimum-required floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)
unset(CMAKE_POLICY_VERSION_MINIMUM)

add_subdirectory(sqli)
add_subdirectory(driver)

option(IFX_ENABLE_INTEGRATION_TESTS
       "Build/run tests that require a live Informix server (see tests/integration)" OFF)
if(IFX_ENABLE_INTEGRATION_TESTS)
  add_subdirectory(integration)
endif()
