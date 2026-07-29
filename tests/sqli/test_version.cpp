#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "sqli/version.h"

TEST_CASE("library version is reported") {
    const auto v = ifxsqli::version();
    CHECK(v.major == 0);
    CHECK(v.minor == 1);
    CHECK(v.patch == 0);
}
