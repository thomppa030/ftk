#include "ftk/base/string_utils.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("A size in bytes reads in the largest unit it fills", "[string_utils]") {
    CHECK(ftk::size_text(0) == "0 B");
    CHECK(ftk::size_text(1023) == "1023 B");
    CHECK(ftk::size_text(1024) == "1.0 KiB");
    CHECK(ftk::size_text(1536) == "1.5 KiB");
    CHECK(ftk::size_text(5'557'452) == "5.3 MiB");
    CHECK(ftk::size_text(2ULL << 30) == "2.0 GiB");
    CHECK(ftk::size_text(3ULL << 40) == "3.0 TiB");
    CHECK(ftk::size_text(2048ULL << 40) == "2048.0 TiB");
}

TEST_CASE("A count's digits stand in threes", "[string_utils]") {
    CHECK(ftk::group_digits(0) == "0");
    CHECK(ftk::group_digits(999) == "999");
    CHECK(ftk::group_digits(1000) == "1,000");
    CHECK(ftk::group_digits(691'263) == "691,263");
    CHECK(ftk::group_digits(12'345'678) == "12,345,678");
}
