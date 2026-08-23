// Proves `pio test -e native` is a working command, not a promise.
//
// A scaffold test that asserted `true` would pass on a broken toolchain and tell nobody. These
// assert something that can actually be wrong: that the pure header compiles on a host, that
// its constants are what they claim, and -- the point of the whole `native` environment -- that
// C++17 is really in effect, since the pure modules that follow are written in it.

#include <unity.h>

#include <string>
#include <string_view>

#include "pure/version.h"

void setUp() {}
void tearDown() {}

static void test_the_pure_header_compiles_and_is_populated() {
    TEST_ASSERT_NOT_NULL(roboface::kFirmwareVersion);
    TEST_ASSERT_NOT_NULL(roboface::kBoard);
    TEST_ASSERT_EQUAL_STRING("m5stack-cores3", roboface::kBoard);
}

static void test_the_version_looks_like_a_version() {
    const std::string version{roboface::kFirmwareVersion};

    TEST_ASSERT_TRUE(version.find('.') != std::string::npos);
    TEST_ASSERT_TRUE(version.size() >= 5);
}

static void test_cxx17_is_actually_in_effect() {
    // The pure modules are written in C++17; if the toolchain quietly fell back to an older
    // standard, everything after this issue would fail in a far less obvious way.
    constexpr std::string_view sample{"roboface"};

    TEST_ASSERT_EQUAL_UINT(8u, sample.size());
    TEST_ASSERT_TRUE(sample.substr(0, 4) == "robo");
    TEST_ASSERT_EQUAL_INT(201703L, __cplusplus > 201402L ? 201703L : 0L);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_pure_header_compiles_and_is_populated);
    RUN_TEST(test_the_version_looks_like_a_version);
    RUN_TEST(test_cxx17_is_actually_in_effect);
    return UNITY_END();
}
