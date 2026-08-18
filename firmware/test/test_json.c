/**
 * Host-side tests for the JSON codec.
 *
 * The OCPP client's entire view of the CSMS goes through this, so a parser bug
 * here shows up as a charger that silently ignores a RemoteStopTransaction.
 * The cases below are real OCPP 1.6J frames wherever possible.
 */
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail;
#define CHECK(cond, ...) do {                                   \
    if (!(cond)) { g_fail++;                                    \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__); printf("\n"); }                    \
} while (0)

static void test_parse_basic(void)
{
    printf("json_parse basics\n");
    json_tok_t t[64];

    const char *js = "{\"a\":1,\"b\":\"two\",\"c\":true,\"d\":null,\"e\":[1,2,3]}";
    int n = json_parse(js, strlen(js), t, 64);
    CHECK(n > 0, "parse returned %d", n);
    CHECK(t[0].type == JSON_OBJECT, "root is an object");

    int a = json_object_get(js, t, n, 0, "a");
    int32_t iv = 0;
    CHECK(a > 0 && json_get_int(js, &t[a], &iv) && iv == 1, "a == 1");

    int b = json_object_get(js, t, n, 0, "b");
    char buf[32];
    json_copy_string(js, &t[b], buf, sizeof(buf));
    CHECK(strcmp(buf, "two") == 0, "b == \"two\", got '%s'", buf);

    int c = json_object_get(js, t, n, 0, "c");
    bool bv = false;
    CHECK(json_get_bool(js, &t[c], &bv) && bv, "c is true");

    int d = json_object_get(js, t, n, 0, "d");
    CHECK(json_is_null(js, &t[d]), "d is null");

    int e = json_object_get(js, t, n, 0, "e");
    CHECK(t[e].type == JSON_ARRAY && t[e].size == 3, "e has 3 elements");
    int e1 = json_array_get(t, n, e, 1);
    CHECK(e1 > 0 && json_get_int(js, &t[e1], &iv) && iv == 2, "e[1] == 2");

    /* A key that is not present must report absent, not return a neighbour. */
    CHECK(json_object_get(js, t, n, 0, "zz") == -1, "absent key returns -1");
}

static void test_nested(void)
{
    printf("json_parse nesting\n");
    json_tok_t t[128];

    /* A real OCPP SetChargingProfile payload. */
    const char *js =
      "{\"connectorId\":1,\"csChargingProfiles\":{\"chargingProfileId\":100,"
      "\"stackLevel\":0,\"chargingProfilePurpose\":\"TxDefaultProfile\","
      "\"chargingProfileKind\":\"Absolute\",\"chargingSchedule\":{"
      "\"chargingRateUnit\":\"A\",\"chargingSchedulePeriod\":["
      "{\"startPeriod\":0,\"limit\":32.0},"
      "{\"startPeriod\":3600,\"limit\":16.0}]}}}";

    int n = json_parse(js, strlen(js), t, 128);
    CHECK(n > 0, "parse returned %d", n);

    int prof = json_object_get(js, t, n, 0, "csChargingProfiles");
    CHECK(prof > 0, "found csChargingProfiles");

    int purpose = json_object_get(js, t, n, prof, "chargingProfilePurpose");
    CHECK(json_equals(js, &t[purpose], "TxDefaultProfile"), "purpose");

    int sched = json_object_get(js, t, n, prof, "chargingSchedule");
    CHECK(sched > 0, "found chargingSchedule");

    int periods = json_object_get(js, t, n, sched, "chargingSchedulePeriod");
    CHECK(periods > 0 && t[periods].size == 2, "2 schedule periods");

    int p1 = json_array_get(t, n, periods, 1);
    int lim = json_object_get(js, t, n, p1, "limit");
    float f = 0;
    CHECK(json_get_float(js, &t[lim], &f) && fabsf(f - 16.0f) < 0.001f,
          "second period limit is 16 A, got %f", (double)f);

    /* Nested lookup must not leak across siblings: "chargingRateUnit" lives in
     * chargingSchedule, so asking the root for it must fail. */
    CHECK(json_object_get(js, t, n, 0, "chargingRateUnit") == -1,
          "nested key not visible from the root");
}

static void test_ocpp_frame(void)
{
    printf("OCPP CALL frame\n");
    json_tok_t t[64];
    const char *js =
      "[2,\"msg-1\",\"RemoteStartTransaction\",{\"idTag\":\"ABC123\",\"connectorId\":1}]";

    int n = json_parse(js, strlen(js), t, 64);
    CHECK(n > 0 && t[0].type == JSON_ARRAY, "frame is an array");
    CHECK(t[0].size == 4, "CALL has 4 elements, got %d", t[0].size);

    int32_t type = 0;
    int e0 = json_array_get(t, n, 0, 0);
    CHECK(json_get_int(js, &t[e0], &type) && type == 2, "message type 2 = CALL");

    int e2 = json_array_get(t, n, 0, 2);
    CHECK(json_equals(js, &t[e2], "RemoteStartTransaction"), "action name");

    int e3 = json_array_get(t, n, 0, 3);
    CHECK(t[e3].type == JSON_OBJECT, "payload is an object");
    int tag = json_object_get(js, t, n, e3, "idTag");
    char buf[32];
    json_copy_string(js, &t[tag], buf, sizeof(buf));
    CHECK(strcmp(buf, "ABC123") == 0, "idTag, got '%s'", buf);
}

static void test_malformed(void)
{
    printf("json_parse rejects malformed input\n");
    json_tok_t t[32];

    /* Truncation must be reported, never treated as a complete document —
     * a partial WebSocket frame is a normal occurrence. */
    const char *cut = "{\"a\":1,\"b\":";
    CHECK(json_parse(cut, strlen(cut), t, 32) < 0, "truncated object rejected");

    const char *cut2 = "{\"unterminated";
    CHECK(json_parse(cut2, strlen(cut2), t, 32) < 0, "unterminated string rejected");

    const char *mismatch = "{\"a\":[1,2}";
    CHECK(json_parse(mismatch, strlen(mismatch), t, 32) < 0, "mismatched bracket rejected");

    /* Running out of tokens must fail loudly, not silently truncate the view. */
    const char *big = "[1,2,3,4,5,6,7,8,9,10]";
    CHECK(json_parse(big, strlen(big), t, 4) == JSON_ERR_NOMEM, "token exhaustion");
}

static void test_escapes(void)
{
    printf("string escapes\n");
    json_tok_t t[16];
    const char *js = "{\"s\":\"a\\\"b\\\\c\\nd\\u0041e\"}";
    int n = json_parse(js, strlen(js), t, 16);
    CHECK(n > 0, "parse ok");

    int s = json_object_get(js, t, n, 0, "s");
    char buf[32];
    json_copy_string(js, &t[s], buf, sizeof(buf));
    CHECK(strcmp(buf, "a\"b\\c\ndAe") == 0, "escapes decoded, got '%s'", buf);

    /* Truncation must still leave a valid NUL-terminated string. */
    char small[5];
    json_copy_string(js, &t[s], small, sizeof(small));
    CHECK(strlen(small) < sizeof(small), "truncated output stays terminated");
}

static void test_writer(void)
{
    printf("json writer\n");
    char buf[256];
    json_writer_t w;

    json_writer_init(&w, buf, sizeof(buf));
    json_obj_open(&w);
    json_kv_str(&w, "status", "Accepted");
    json_kv_int(&w, "interval", 300);
    json_kv_bool(&w, "ok", true);
    json_kv_float(&w, "limit", 32.0f, 1);
    json_obj_close(&w);
    CHECK(json_writer_finish(&w) > 0, "writer succeeded");
    CHECK(strcmp(buf, "{\"status\":\"Accepted\",\"interval\":300,"
                      "\"ok\":true,\"limit\":32.0}") == 0,
          "flat object, got %s", buf);

    /* Nested structures must not emit stray or missing commas — this is the
     * classic append-only-writer bug. */
    json_writer_init(&w, buf, sizeof(buf));
    json_arr_open(&w);
    json_int(&w, 2);
    json_str(&w, "id");
    json_str(&w, "BootNotification");
    json_obj_open(&w);
    json_kv_str(&w, "chargePointVendor", "EVREST");
    json_key(&w, "nested");
    json_obj_open(&w);
    json_kv_int(&w, "x", 1);
    json_obj_close(&w);
    json_key(&w, "list");
    json_arr_open(&w);
    json_int(&w, 1);
    json_int(&w, 2);
    json_arr_close(&w);
    json_obj_close(&w);
    json_arr_close(&w);
    CHECK(json_writer_finish(&w) > 0, "nested writer succeeded");
    CHECK(strcmp(buf, "[2,\"id\",\"BootNotification\","
                      "{\"chargePointVendor\":\"EVREST\","
                      "\"nested\":{\"x\":1},\"list\":[1,2]}]") == 0,
          "nested output, got %s", buf);

    /* Whatever the writer emits, the parser must accept. */
    json_tok_t t[64];
    CHECK(json_parse(buf, strlen(buf), t, 64) > 0, "round-trips through the parser");

    /* Overflow must be sticky and detectable, not produce truncated JSON that
     * looks valid. */
    char tiny[16];
    json_writer_init(&w, tiny, sizeof(tiny));
    json_obj_open(&w);
    json_kv_str(&w, "a_very_long_key_indeed", "and_a_long_value");
    json_obj_close(&w);
    CHECK(json_writer_finish(&w) == 0, "overflow reported as 0 length");

    /* Values needing escapes must survive a round trip. */
    json_writer_init(&w, buf, sizeof(buf));
    json_obj_open(&w);
    json_kv_str(&w, "q", "he said \"hi\"\nnewline\\slash");
    json_obj_close(&w);
    CHECK(json_writer_finish(&w) > 0, "escaped write ok");
    int n = json_parse(buf, strlen(buf), t, 64);
    CHECK(n > 0, "escaped output re-parses");
    int q = json_object_get(buf, t, n, 0, "q");
    char back[64];
    json_copy_string(buf, &t[q], back, sizeof(back));
    CHECK(strcmp(back, "he said \"hi\"\nnewline\\slash") == 0,
          "escape round trip, got '%s'", back);
}

int main(void)
{
    printf("=== JSON codec tests ===\n");
    test_parse_basic();
    test_nested();
    test_ocpp_frame();
    test_malformed();
    test_escapes();
    test_writer();
    if (g_fail == 0) { printf("\nAll tests passed.\n"); return 0; }
    printf("\n%d test(s) FAILED.\n", g_fail);
    return 1;
}
