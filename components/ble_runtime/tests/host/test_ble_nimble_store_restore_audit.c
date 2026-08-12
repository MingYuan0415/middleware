#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ble_nimble_store_restore_audit.h"

#define TEST_MAX_KEYS 12U
#define TEST_MAX_COUNTS 4U
#define TEST_MAX_MATCHES 4U

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
    const char *matches[TEST_MAX_MATCHES];
    size_t match_count;
    const char *failing_key;
    esp_err_t probe_result;
    const char *failing_match_key;
    esp_err_t match_result;
    size_t probe_calls;
    size_t count_calls;
    size_t match_calls;
    size_t mismatch_calls;
    ble_nimble_store_restore_mismatch_t last_mismatch;
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

static esp_err_t _match(
    void *context, int object_type, const char *key, bool *out_match)
{
    fake_audit_t *fake = context;

    assert(fake != NULL);
    assert(object_type != 0);
    assert(key != NULL);
    assert(out_match != NULL);
    fake->match_calls++;
    if (fake->failing_match_key != NULL &&
            strcmp(fake->failing_match_key, key) == 0)
    {
        return fake->match_result;
    }
    *out_match = false;
    for (size_t i = 0U; i < fake->match_count; ++i)
    {
        if (strcmp(fake->matches[i], key) == 0)
        {
            *out_match = true;
            break;
        }
    }
    return ESP_OK;
}

static void _mismatch(
    void *context, const ble_nimble_store_restore_mismatch_t *mismatch)
{
    fake_audit_t *fake = context;

    assert(fake != NULL);
    assert(mismatch != NULL);
    fake->mismatch_calls++;
    fake->last_mismatch = *mismatch;
}

static const ble_nimble_store_restore_family_t s_families[] =
{
    {"our_sec", 1U, 11, false},
    {"peer_sec", 1U, 12, false},
    {"cccd_sec", 3U, 13, false},
};

static const ble_nimble_store_restore_family_t s_exact_family =
{
    "rpa_rec", 2U, 14, true,
};

static ble_nimble_store_restore_audit_ops_t _ops(fake_audit_t *fake)
{
    const ble_nimble_store_restore_audit_ops_t ops =
    {
        .probe = _probe,
        .count = _count,
        .match = _match,
        .mismatch = _mismatch,
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

static void test_exact_entries_accept_absent_and_matching_records(void)
{
    fake_audit_t fake = {0};
    const ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);

    assert(ble_nimble_store_restore_audit(
               &s_exact_family, 1U, &ops) == ESP_OK);
    assert(fake.probe_calls == 2U);
    assert(fake.match_calls == 0U);
    assert(fake.count_calls == 0U);

    fake.keys[0] = "rpa_rec_1";
    fake.key_count = 1U;
    fake.matches[0] = "rpa_rec_1";
    fake.match_count = 1U;
    fake.probe_calls = 0U;

    assert(ble_nimble_store_restore_audit(
               &s_exact_family, 1U, &ops) == ESP_OK);
    assert(fake.probe_calls == 2U);
    assert(fake.match_calls == 1U);
    assert(fake.count_calls == 0U);
}

static void test_exact_entry_missing_or_malformed_fails_with_diagnostic(void)
{
    fake_audit_t fake =
    {
        .keys = {"rpa_rec_2"},
        .key_count = 1U,
    };
    const ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);

    /* A present durable blob that is absent, malformed, or different in RAM
     * is reported by the production matcher as an exact mismatch. */
    assert(ble_nimble_store_restore_audit(
               &s_exact_family, 1U, &ops) == ESP_ERR_INVALID_STATE);
    assert(fake.match_calls == 1U);
    assert(fake.mismatch_calls == 1U);
    assert(strcmp(fake.last_mismatch.key_prefix, "rpa_rec") == 0);
    assert(fake.last_mismatch.durable_count == 1U);
    assert(fake.last_mismatch.restored_count == 0U);
}

static void test_exact_entry_access_error_stops_the_audit(void)
{
    fake_audit_t fake =
    {
        .keys = {"rpa_rec_1"},
        .key_count = 1U,
        .failing_match_key = "rpa_rec_1",
        .match_result = ESP_FAIL,
    };
    const ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);

    assert(ble_nimble_store_restore_audit(
               &s_exact_family, 1U, &ops) == ESP_FAIL);
    assert(fake.match_calls == 1U);
    assert(fake.mismatch_calls == 0U);
}

static void test_invalid_configuration_is_rejected(void)
{
    fake_audit_t fake = {0};
    const ble_nimble_store_restore_audit_ops_t ops = _ops(&fake);
    const ble_nimble_store_restore_family_t long_key =
    {
        "key_prefix_too_long", 1U, 1, false,
    };

    assert(ble_nimble_store_restore_audit(NULL, 1U, &ops) ==
           ESP_ERR_INVALID_ARG);
    assert(ble_nimble_store_restore_audit(s_families, 0U, &ops) ==
           ESP_ERR_INVALID_ARG);
    assert(ble_nimble_store_restore_audit(&long_key, 1U, &ops) ==
           ESP_ERR_INVALID_ARG);
    ble_nimble_store_restore_audit_ops_t missing_op = ops;

    missing_op.count = NULL;
    assert(ble_nimble_store_restore_audit(
               s_families, 1U, &missing_op) == ESP_ERR_INVALID_ARG);
    missing_op = ops;
    missing_op.match = NULL;
    assert(ble_nimble_store_restore_audit(
               &s_exact_family, 1U, &missing_op) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_matching_empty_and_populated_store();
    test_incomplete_security_restore_fails_closed();
    test_hidden_cccd_fails_before_revoke_confirmation();
    test_malformed_blob_is_visible_as_count_mismatch();
    test_access_errors_stop_the_audit();
    test_reset_resync_reaudits_the_fresh_mirror();
    test_exact_entries_accept_absent_and_matching_records();
    test_exact_entry_missing_or_malformed_fails_with_diagnostic();
    test_exact_entry_access_error_stops_the_audit();
    test_invalid_configuration_is_rejected();
    puts("ble_nimble_store_restore_audit host tests passed");
    return 0;
}
