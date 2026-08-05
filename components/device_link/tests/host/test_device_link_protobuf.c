#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <protobuf-c/protobuf-c.h>

#include "microtech/link/v1/envelope.pb-c.h"

#define TEST_ASSERT_TRUE(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            fprintf(stderr, "assertion failed at line %d: %s\n", \
                    __LINE__, #condition); \
            abort(); \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL(expected, actual) \
    do \
    { \
        const long expected_value = (long)(expected); \
        const long actual_value = (long)(actual); \
        if (expected_value != actual_value) \
        { \
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%ld != %ld)\n", \
                    __LINE__, #expected, #actual, expected_value, actual_value); \
            abort(); \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_U64(expected, actual) \
    do \
    { \
        const uint64_t expected_value = (uint64_t)(expected); \
        const uint64_t actual_value = (uint64_t)(actual); \
        if (expected_value != actual_value) \
        { \
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%llu != %llu)\n", \
                    __LINE__, #expected, #actual, \
                    (unsigned long long)expected_value, \
                    (unsigned long long)actual_value); \
            abort(); \
        } \
    } while (0)

static void test_capabilities_request_roundtrip(void)
{
    Microtech__Link__V1__Envelope envelope =
        MICROTECH__LINK__V1__ENVELOPE__INIT;
    Microtech__Link__V1__Envelope *parsed;
    uint8_t buffer[64];
    size_t packed_size;
    size_t written;

    envelope.protocol_major = 1U;
    envelope.protocol_minor = 0U;
    envelope.boot_id = UINT64_C(0x0102030405060708);
    envelope.body_case = MICROTECH__LINK__V1__ENVELOPE__BODY_REQUEST;
    envelope.request = malloc(sizeof(*envelope.request));
    TEST_ASSERT_TRUE(envelope.request != NULL);
    microtech__link__v1__request__init(envelope.request);
    envelope.request->request_id = 1U;
    envelope.request->body_case =
        MICROTECH__LINK__V1__REQUEST__BODY_GET_CAPABILITIES;
    envelope.request->get_capabilities =
        malloc(sizeof(*envelope.request->get_capabilities));
    TEST_ASSERT_TRUE(envelope.request->get_capabilities != NULL);
    microtech__link__v1__get_capabilities_request__init(
        envelope.request->get_capabilities);

    packed_size = microtech__link__v1__envelope__get_packed_size(&envelope);
    TEST_ASSERT_TRUE(packed_size <= sizeof(buffer));
    written = microtech__link__v1__envelope__pack(&envelope, buffer);
    TEST_ASSERT_EQUAL_U64(packed_size, written);
    TEST_ASSERT_EQUAL_U64(24U, written);
    {
        const uint8_t golden[] =
        {
            0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
            0x03, 0x02, 0x01, 0x52, 0x0B, 0x09, 0x01, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
        };
        TEST_ASSERT_TRUE(memcmp(buffer, golden, sizeof(golden)) == 0);
    }

    parsed = microtech__link__v1__envelope__unpack(
                 NULL, written, buffer);
    TEST_ASSERT_TRUE(parsed != NULL);
    TEST_ASSERT_EQUAL_U64(1U, parsed->protocol_major);
    TEST_ASSERT_EQUAL_U64(0U, parsed->protocol_minor);
    TEST_ASSERT_EQUAL_U64(UINT64_C(0x0102030405060708), parsed->boot_id);
    TEST_ASSERT_TRUE(parsed->request != NULL);
    TEST_ASSERT_EQUAL_U64(1U, parsed->request->request_id);
    TEST_ASSERT_EQUAL(MICROTECH__LINK__V1__REQUEST__BODY_GET_CAPABILITIES,
                      parsed->request->body_case);
    TEST_ASSERT_TRUE(parsed->request->get_capabilities != NULL);
    TEST_ASSERT_EQUAL(MICROTECH__LINK__V1__ENVELOPE__BODY_REQUEST,
                      parsed->body_case);
    TEST_ASSERT_TRUE(parsed->response == NULL || parsed->body_case !=
                     MICROTECH__LINK__V1__ENVELOPE__BODY_RESPONSE);

    microtech__link__v1__envelope__free_unpacked(parsed, NULL);
    free(envelope.request->get_capabilities);
    free(envelope.request);
}

static void test_authorize_commit_request_roundtrip(void)
{
    Microtech__Link__V1__Envelope envelope =
        MICROTECH__LINK__V1__ENVELOPE__INIT;
    Microtech__Link__V1__Envelope *parsed;
    uint8_t buffer[64];
    const uint8_t credential_id[16] =
    {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    };
    size_t packed_size;
    size_t written;

    envelope.protocol_major = 1U;
    envelope.protocol_minor = 0U;
    envelope.boot_id = UINT64_C(0x0102030405060708);
    envelope.body_case = MICROTECH__LINK__V1__ENVELOPE__BODY_REQUEST;
    envelope.request = malloc(sizeof(*envelope.request));
    TEST_ASSERT_TRUE(envelope.request != NULL);
    microtech__link__v1__request__init(envelope.request);
    envelope.request->request_id = 2U;
    envelope.request->body_case =
        MICROTECH__LINK__V1__REQUEST__BODY_AUTHORIZE_COMMIT;
    envelope.request->authorize_commit =
        malloc(sizeof(*envelope.request->authorize_commit));
    TEST_ASSERT_TRUE(envelope.request->authorize_commit != NULL);
    microtech__link__v1__authorize_commit_request__init(
        envelope.request->authorize_commit);
    envelope.request->authorize_commit->authorization_txn_id =
        UINT64_C(0x0a0b0c0d0e0f1011);
    envelope.request->authorize_commit->credential_id.data =
        (uint8_t *)credential_id;
    envelope.request->authorize_commit->credential_id.len =
        sizeof(credential_id);

    packed_size = microtech__link__v1__envelope__get_packed_size(&envelope);
    TEST_ASSERT_TRUE(packed_size <= sizeof(buffer));
    written = microtech__link__v1__envelope__pack(&envelope, buffer);
    TEST_ASSERT_EQUAL_U64(packed_size, written);
    TEST_ASSERT_EQUAL_U64(51U, written);
    {
        const uint8_t golden[] =
        {
            0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
            0x03, 0x02, 0x01, 0x52, 0x26, 0x09, 0x02, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6A, 0x1B,
            0x09, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
            0x0A, 0x12, 0x10, 0x01, 0x02, 0x03, 0x04, 0x05,
            0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
            0x0E, 0x0F, 0x10,
        };
        TEST_ASSERT_TRUE(memcmp(buffer, golden, sizeof(golden)) == 0);
    }

    parsed = microtech__link__v1__envelope__unpack(
                 NULL, written, buffer);
    TEST_ASSERT_TRUE(parsed != NULL);
    TEST_ASSERT_TRUE(parsed->request != NULL);
    TEST_ASSERT_TRUE(parsed->request->authorize_commit != NULL);
    TEST_ASSERT_EQUAL(MICROTECH__LINK__V1__REQUEST__BODY_AUTHORIZE_COMMIT,
                      parsed->request->body_case);
    TEST_ASSERT_EQUAL_U64(UINT64_C(0x0a0b0c0d0e0f1011),
                          parsed->request->authorize_commit->authorization_txn_id);
    TEST_ASSERT_EQUAL_U64(sizeof(credential_id),
                          parsed->request->authorize_commit->credential_id.len);
    TEST_ASSERT_TRUE(memcmp(
                         parsed->request->authorize_commit->credential_id.data,
                         credential_id, sizeof(credential_id)) == 0);

    microtech__link__v1__envelope__free_unpacked(parsed, NULL);
    free(envelope.request->authorize_commit);
    free(envelope.request);
}

static void test_transfer_start_roundtrip(void)
{
    Microtech__Link__V1__Envelope envelope =
        MICROTECH__LINK__V1__ENVELOPE__INIT;
    Microtech__Link__V1__Envelope *parsed;
    uint8_t buffer[128];
    const uint8_t digest[32] = {0xAB};
    size_t packed_size;
    size_t written;

    envelope.protocol_major = 1U;
    envelope.protocol_minor = 0U;
    envelope.boot_id = UINT64_C(0x0102030405060708);
    envelope.body_case = MICROTECH__LINK__V1__ENVELOPE__BODY_TRANSFER_CONTROL;
    envelope.transfer_control =
        malloc(sizeof(*envelope.transfer_control));
    TEST_ASSERT_TRUE(envelope.transfer_control != NULL);
    microtech__link__v1__transfer_control__init(envelope.transfer_control);
    envelope.transfer_control->transfer_id = UINT64_C(0x1122334455667788);
    envelope.transfer_control->body_case =
        MICROTECH__LINK__V1__TRANSFER_CONTROL__BODY_START;
    envelope.transfer_control->start =
        malloc(sizeof(*envelope.transfer_control->start));
    TEST_ASSERT_TRUE(envelope.transfer_control->start != NULL);
    microtech__link__v1__transfer_start__init(envelope.transfer_control->start);
    envelope.transfer_control->start->type =
        MICROTECH__LINK__V1__TRANSFER_TYPE__TRANSFER_TYPE_TEST_FILE;
    envelope.transfer_control->start->total_length = UINT64_C(65536);
    envelope.transfer_control->start->sha256.data = (uint8_t *)digest;
    envelope.transfer_control->start->sha256.len = sizeof(digest);

    packed_size = microtech__link__v1__envelope__get_packed_size(&envelope);
    TEST_ASSERT_TRUE(packed_size <= sizeof(buffer));
    written = microtech__link__v1__envelope__pack(&envelope, buffer);
    TEST_ASSERT_EQUAL_U64(packed_size, written);
    TEST_ASSERT_EQUAL_U64(69U, written);
    {
        const uint8_t golden_prefix[] =
        {
            0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
            0x03, 0x02, 0x01, 0x72, 0x38, 0x09, 0x88, 0x77,
            0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x52, 0x2D,
            0x08, 0x01, 0x11, 0x00, 0x00, 0x01, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x1A, 0x20,
        };
        TEST_ASSERT_TRUE(memcmp(buffer, golden_prefix,
                                sizeof(golden_prefix)) == 0);
        TEST_ASSERT_TRUE(memcmp(buffer + sizeof(golden_prefix), digest,
                                sizeof(digest)) == 0);
    }

    parsed = microtech__link__v1__envelope__unpack(
                 NULL, written, buffer);
    TEST_ASSERT_TRUE(parsed != NULL);
    TEST_ASSERT_TRUE(parsed->transfer_control != NULL);
    TEST_ASSERT_EQUAL(MICROTECH__LINK__V1__ENVELOPE__BODY_TRANSFER_CONTROL,
                      parsed->body_case);
    TEST_ASSERT_EQUAL(MICROTECH__LINK__V1__TRANSFER_CONTROL__BODY_START,
                      parsed->transfer_control->body_case);
    TEST_ASSERT_TRUE(parsed->transfer_control->start != NULL);
    TEST_ASSERT_EQUAL_U64(UINT64_C(0x1122334455667788),
                          parsed->transfer_control->transfer_id);
    TEST_ASSERT_EQUAL_U64(65536U,
                          parsed->transfer_control->start->total_length);
    TEST_ASSERT_EQUAL_U64(sizeof(digest),
                          parsed->transfer_control->start->sha256.len);

    microtech__link__v1__envelope__free_unpacked(parsed, NULL);
    free(envelope.transfer_control->start);
    free(envelope.transfer_control);
}

int main(void)
{
    test_capabilities_request_roundtrip();
    test_authorize_commit_request_roundtrip();
    test_transfer_start_roundtrip();
    printf("device_link_protobuf: all tests passed\n");
    return 0;
}
