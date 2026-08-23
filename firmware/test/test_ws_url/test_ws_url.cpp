// Host tests for the SERVER_URL parser.
//
// It lives in pure/ so a typo in config.h is something a laptop catches, not something you learn
// by flashing a board and watching nothing connect.

#include <unity.h>

#include "pure/ws_url.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

static void test_the_default_config_url_parses() {
    // Exactly what config.example.h ships.
    const WsUrl url = parseWsUrl("ws://192.168.1.64:8000/ws");

    TEST_ASSERT_TRUE(url.valid);
    TEST_ASSERT_FALSE(url.secure);
    TEST_ASSERT_EQUAL_STRING("192.168.1.64", url.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(8000, url.port);
    TEST_ASSERT_EQUAL_STRING("/ws", url.path.c_str());
}

static void test_a_hostname_works_as_well_as_an_address() {
    const WsUrl url = parseWsUrl("ws://roboface.local:8000/ws");

    TEST_ASSERT_TRUE(url.valid);
    TEST_ASSERT_EQUAL_STRING("roboface.local", url.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(8000, url.port);
}

static void test_a_missing_port_uses_the_scheme_default() {
    // Not 8000. A URL without a port means the well-known one, and quietly guessing the product's
    // port would hide a missing ":8000" in config.h -- which is exactly the typo this catches.
    TEST_ASSERT_EQUAL_UINT16(80, parseWsUrl("ws://host/ws").port);
    TEST_ASSERT_EQUAL_UINT16(443, parseWsUrl("wss://host/ws").port);
}

static void test_a_missing_path_becomes_root() {
    const WsUrl url = parseWsUrl("ws://host:8000");

    TEST_ASSERT_TRUE(url.valid);
    TEST_ASSERT_EQUAL_STRING("/", url.path.c_str());
}

static void test_wss_is_accepted_and_marked() {
    // The server has no TLS yet. Refusing the scheme outright would be pedantry against a URL
    // that is aspirationally correct; marking it lets ws.cpp say the transport is still plaintext
    // rather than silently downgrading.
    const WsUrl url = parseWsUrl("wss://host:8443/ws");

    TEST_ASSERT_TRUE(url.valid);
    TEST_ASSERT_TRUE(url.secure);
    TEST_ASSERT_EQUAL_UINT16(8443, url.port);
}

static void test_a_url_that_is_not_a_websocket_url_is_rejected() {
    for (const char* bad : {"http://host:8000/ws", "192.168.1.64:8000", "just-a-hostname", "", "//host"}) {
        TEST_ASSERT_FALSE_MESSAGE(parseWsUrl(bad).valid, bad);
    }
}

static void test_a_null_url_is_rejected_rather_than_crashing() {
    TEST_ASSERT_FALSE(parseWsUrl(nullptr).valid);
}

static void test_an_out_of_range_port_is_rejected() {
    // ":0" and ":70000" both used to become a uint16 nobody would notice -- a connection attempt
    // to port 0 or to a wrapped-around number, retried forever.
    TEST_ASSERT_FALSE(parseWsUrl("ws://host:0/ws").valid);
    TEST_ASSERT_FALSE(parseWsUrl("ws://host:70000/ws").valid);
    TEST_ASSERT_FALSE(parseWsUrl("ws://host:-1/ws").valid);
}

static void test_a_non_numeric_port_is_rejected() {
    TEST_ASSERT_FALSE(parseWsUrl("ws://host:eight/ws").valid);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_default_config_url_parses);
    RUN_TEST(test_a_hostname_works_as_well_as_an_address);
    RUN_TEST(test_a_missing_port_uses_the_scheme_default);
    RUN_TEST(test_a_missing_path_becomes_root);
    RUN_TEST(test_wss_is_accepted_and_marked);
    RUN_TEST(test_a_url_that_is_not_a_websocket_url_is_rejected);
    RUN_TEST(test_a_null_url_is_rejected_rather_than_crashing);
    RUN_TEST(test_an_out_of_range_port_is_rejected);
    RUN_TEST(test_a_non_numeric_port_is_rejected);
    return UNITY_END();
}
