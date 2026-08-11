#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ble_nimble_store_restore_audit.h"

#define TEST_MAX_KEYS 12U
#define TEST_MAX_COUNTS 4U

typedef struct fake_count
{
    int object_type;
    size_t count;
    esp_err_t result;
} fake_count_t;

typedef struct fake_audit
{
    const char *keys[TEST_MAX_KEYS];
    size_t key_count;
    fake_count_t counts[TEST_MAX_COUNTS];
    size_t count_count;
    const char *failing_key;
    esp_err_t probe_result;
    size_t probe_calls;
    size_t count_calls;
} fake_audit_t;

static esp_err_t _probe(
    void *context, const char *key, bool *out_present)
{
    fake_audit_t *fake = context;

    assert(fake != NULL);
    assert(key != NULL);
    assert(out_present != NULL);
    fake->probe_calls++;
    if (fake->failing_key != NULL && strcmp(fake->failing_key, key) == 0)
    {
        return fake->probe_result;
    }
    *out_present = false;
    for (size_t i = 0U; i < fake->key_count; ++i)
    {
        if (strcmp(fake->keys[i], key) == 0)
        {
            *out_present = true;
            break;
        }
    }
    return ESP_OK;
}

static esp_err_t _count(
    void *context, int object_type, size_t *out_count)
{
    fake_audit_t *fake = context;

    assert(fake != NULL);
    assert(out_count != NULL);
    fake->count_calls++;
    for (size_t i = 0U; i < fake->count_count; ++i)
    {
        if (fake->counts[i].object_type == object_type)
        {
            *out_count = fake->counts[i].count;
            return fake->counts[i].result;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static const ble_nimble_store_restore_family_t s_families[] =
{
    {"our_sec", 1U, 11},
    {"peer_sec", 1U, 12},
    {"cccd_sec", 3U, 13},
};

static ble_nimble_store_restore_audit_ops_t _ops(fake_audit_t *fake)
{
    const ble_nimble_store_restore_audit_ops_t ops =
    {
        .probe = _probe,
        .count = _count,
        .context = fake,
    };

    return ops;
}

static void test_matching_empty_and_populated_store(void)
{
    fake_audit_t fake =
    {
        .counts =
        {
            {.object_type = 11, .result = ESP_OK},
            {.object_type = 12, .result = ESP_OK},
            {.object_type = 13, .result = ESP_OK},
        },
        .count_count = 3U,
    };
    ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);

    assert(ble_nimble_store_restore_audit(
               s_families, sizeof(s_families) / sizeof(s_families[0]),
               &ops) == ESP_OK);
    assert(fake.probe_calls == 5U);
    assert(fake.count_calls == 3U);

    fake.keys[0] = "our_sec_1";
    fake.keys[1] = "peer_sec_1";
    fake.keys[2] = "cccd_sec_1";
    fake.keys[3] = "cccd_sec_3";
    fake.key_count = 4U;
    fake.counts[0].count = 1U;
    fake.counts[1].count = 1U;
    fake.counts[2].count = 2U;
    fake.probe_calls = 0U;
    fake.count_calls = 0U;

    assert(ble_nimble_store_restore_audit(
               s_families, sizeof(s_families) / sizeof(s_families[0]),
               &ops) == ESP_OK);
    assert(fake.probe_calls == 5U);
    assert(fake.count_calls == 3U);
}

static void test_incomplete_security_restore_fails_closed(void)
{
    fake_audit_t fake =
    {
        .keys = {"our_sec_1", "peer_sec_1"},
        .key_count = 2U,
        .counts =
        {
            {.object_type = 11, .count = 1U, .result = ESP_OK},
            {.object_type = 12, .count = 0U, .result = ESP_OK},
            {.object_type = 13, .count = 0U, .result = ESP_OK},
        },
        .count_count = 3U,
    };
    const ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);

    assert(ble_nimble_store_restore_audit(
               s_families, sizeof(s_families) / sizeof(s_families[0]),
               &ops) == ESP_ERR_INVALID_STATE);
    assert(fake.count_calls == 2U);
}

static void test_hidden_cccd_fails_before_revoke_confirmation(void)
{
    fake_audit_t fake =
    {
        .keys = {"cccd_sec_1", "cccd_sec_2"},
        .key_count = 2U,
        .counts =
        {
            {.object_type = 11, .result = ESP_OK},
            {.object_type = 12, .result = ESP_OK},
            {.object_type = 13, .count = 1U, .result = ESP_OK},
        },
        .count_count = 3U,
    };
    const ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);

    assert(ble_nimble_store_restore_audit(
               s_families, sizeof(s_families) / sizeof(s_families[0]),
               &ops) == ESP_ERR_INVALID_STATE);
    assert(fake.count_calls == 3U);
}

static void test_malformed_blob_is_visible_as_count_mismatch(void)
{
    fake_audit_t fake =
    {
        /* The durable size probe sees the blob, while fixed IDF rejects its
         * invalid length and therefore leaves the public RAM count at zero. */
        .keys = {"our_sec_1"},
        .key_count = 1U,
        .counts =
        {
            {.object_type = 11, .result = ESP_OK},
            {.object_type = 12, .result = ESP_OK},
            {.object_type = 13, .result = ESP_OK},
        },
        .count_count = 3U,
    };
    const ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);

    assert(ble_nimble_store_restore_audit(
               s_families, sizeof(s_families) / sizeof(s_families[0]),
               &ops) == ESP_ERR_INVALID_STATE);
}

static void test_access_errors_stop_the_audit(void)
{
    fake_audit_t fake =
    {
        .counts =
        {
            {.object_type = 11, .result = ESP_OK},
            {.object_type = 12, .result = ESP_OK},
            {.object_type = 13, .result = ESP_OK},
        },
        .count_count = 3U,
        .failing_key = "our_sec_1",
        .probe_result = ESP_FAIL,
    };
    ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);

    assert(ble_nimble_store_restore_audit(
               s_families, sizeof(s_families) / sizeof(s_families[0]),
               &ops) == ESP_FAIL);
    assert(fake.probe_calls == 1U);
    assert(fake.count_calls == 0U);

    fake.failing_key = NULL;
    fake.counts[0].result = ESP_ERR_NOT_FOUND;
    fake.probe_calls = 0U;

    assert(ble_nimble_store_restore_audit(
               s_families, sizeof(s_families) / sizeof(s_families[0]),
               &ops) == ESP_ERR_NOT_FOUND);
    assert(fake.probe_calls == 1U);
    assert(fake.count_calls == 1U);
}

static void test_reset_resync_reaudits_the_fresh_mirror(void)
{
    fake_audit_t fake =
    {
        .keys = {"our_sec_1", "peer_sec_1"},
        .key_count = 2U,
        .counts =
        {
            {.object_type = 11, .count = 1U, .result = ESP_OK},
            {.object_type = 12, .count = 1U, .result = ESP_OK},
            {.object_type = 13, .result = ESP_OK},
        },
        .count_count = 3U,
    };
    const ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);

    assert(ble_nimble_store_restore_audit(
               s_families, sizeof(s_families) / sizeof(s_families[0]),
               &ops) == ESP_OK);

    /* A host reset clears and reloads the IDF arrays. The same durable keys
     * with a failed second restore must not inherit the first sync's success. */
    fake.counts[0].count = 0U;
    assert(ble_nimble_store_restore_audit(
               s_families, sizeof(s_families) / sizeof(s_families[0]),
               &ops) == ESP_ERR_INVALID_STATE);
}

static void test_invalid_configuration_is_rejected(void)
{
    fake_audit_t fake = {0};
    const ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);
    const ble_nimble_store_restore_family_t long_key =
    {
        "key_prefix_too_long", 1U, 1,
    };

    assert(ble_nimble_store_restore_audit(NULL, 1U, &ops) ==
           ESP_ERR_INVALID_ARG);
    assert(ble_nimble_store_restore_audit(s_families, 0U, &ops) ==
           ESP_ERR_INVALID_ARG);
    assert(ble_nimble_store_restore_audit(&long_key, 1U, &ops) ==
           ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_matching_empty_and_populated_store();
    test_incomplete_security_restore_fails_closed();
    test_hidden_cccd_fails_before_revoke_confirmation();
    test_malformed_blob_is_visible_as_count_mismatch();
    test_access_errors_stop_the_audit();
    test_reset_resync_reaudits_the_fresh_mirror();
    test_invalid_configuration_is_rejected();
    puts("ble_nimble_store_restore_audit host tests passed");
    return 0;
}
