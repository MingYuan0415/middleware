#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_codec.h"
#include "ble_link_dispatcher.h"
#include "ble_link_events.h"
#include "ble_link_reassembler.h"
#include "ble_link_service.h"
#include "ble_link_session.h"
#include "ble_link_state.h"

#define DBG_TAG "ble_link_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define BLE_LINK_SERVICE_PROTOCOL_MAJOR 1U
#define BLE_LINK_SERVICE_PREFERRED_ATT_MTU 498U
#define BLE_LINK_SERVICE_CONTROL_MAX_BYTES 4096U
#define BLE_LINK_SERVICE_SESSION_MAX_BYTES 1024U
#define BLE_LINK_SERVICE_DEVICE_AUTH_ID_BYTES 16U
#define BLE_LINK_SERVICE_PROTOCOMM_PATCH_VERSION 1U

typedef struct ble_link_service
{
    uint64_t boot_id;
    ble_link_service_output_t output;
    void *output_arg;
    ble_link_reassembler_t reassembler[2];
    uint8_t reassembly_buffer[2][BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES];
    uint32_t feed_generation;
    uint16_t outbound_frame_id;
    uint32_t next_subscription_id;
    struct
    {
        bool active;
        uint32_t generation;
        uint32_t subscription_id;
        uint64_t sequence_baseline;
        uint8_t event_key[BLE_LINK_SERVICE_EVENT_KEY_BYTES];
        uint8_t nonce_prefix[BLE_LINK_SERVICE_NONCE_PREFIX_BYTES];
    } subscriber;
    struct
    {
        bool active;
        bool committed;
        uint64_t authorization_txn_id;
        uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
    } auth_txn;
    ble_link_service_facts_t current_facts;
    ble_link_service_rx_channel_t current_channel;
    bool authorize_enabled;
    unsigned int pending_transactions;
    size_t max_pending_frames;
} ble_link_service_t;

static ble_link_service_t s_service;

bool ble_link_service_response_in_flight(void)
{
    return s_service.pending_transactions > 0U;
}

void ble_link_service_response_completed(void)
{
    if (s_service.pending_transactions > 0U)
    {
        s_service.pending_transactions--;
    }
}

void ble_link_service_abort_transactions(void)
{
    s_service.pending_transactions = 0U;
}

/**
 * @brief Abort the current control/session flow: clear the reassembly
 * slot, subscriber, authorization transaction, and close the Security 2
 * session (framing contract).
 */
static void _ble_link_service_abort_session(uint32_t generation)
{
    /* Only a current generation may clear state; a stale timeout or feed
     * has no effect. */
    if (ble_link_session_security2_close_current(generation) != ESP_OK)
    {
        return;
    }
    s_service.pending_transactions = 0U;
    ble_link_reassembler_reset(&s_service.reassembler[0]);
    ble_link_reassembler_reset(&s_service.reassembler[1]);
    s_service.subscriber.active = false;
    s_service.auth_txn.active = false;
    s_service.auth_txn.committed = false;
    ble_link_dispatcher_clear_session();
}

static void _ble_link_service_write_varint(uint8_t *out, size_t *pos,
        uint64_t value)
{
    while (value >= 0x80U)
    {
        out[(*pos)++] = (uint8_t)(value & 0x7fU) | 0x80U;
        value >>= 7U;
    }
    out[(*pos)++] = (uint8_t)value;
}

static void _ble_link_service_write_tag(uint8_t *out, size_t *pos,
                                        uint32_t field, uint32_t wire)
{
    _ble_link_service_write_varint(out, pos,
                                   ((uint64_t)field << 3U) | wire);
}

static void _ble_link_service_write_fixed64(uint8_t *out, size_t *pos,
        uint64_t value)
{
    for (unsigned int i = 0U; i < 8U; ++i)
    {
        out[(*pos)++] = (uint8_t)(value >> (8U * i));
    }
}

static void _ble_link_service_write_fixed32(uint8_t *out, size_t *pos,
        uint32_t value)
{
    for (unsigned int i = 0U; i < 4U; ++i)
    {
        out[(*pos)++] = (uint8_t)(value >> (8U * i));
    }
}

static void _ble_link_service_write_bytes(uint8_t *out, size_t *pos,
        const uint8_t *data, size_t len)
{
    _ble_link_service_write_varint(out, pos, len);
    memcpy(&out[*pos], data, len);
    *pos += len;
}

static size_t _ble_link_service_varint_size(uint64_t value)
{
    size_t size = 1U;

    while (value >= 0x80U)
    {
        value >>= 7U;
        size++;
    }
    return size;
}

/**
 * @brief Build the current LinkState from session and connection facts.
 */
static void _ble_link_service_build_link_state(
    const ble_link_service_facts_t *facts,
    ble_link_state_snapshot_t *out)
{
    const uint32_t flags = ble_link_session_get_state_flags();

    memset(out, 0, sizeof(*out));
    out->boot_id = facts->active_boot_id;
    if ((flags & BLE_LINK_STATE_FLAG_BOUND) != 0U)
    {
        out->binding_state = BLE_LINK_BINDING_BOUND;
    }
    else if ((flags & BLE_LINK_STATE_FLAG_BINDABLE) != 0U)
    {
        out->binding_state = BLE_LINK_BINDING_PAIRING_WINDOW;
    }
    else
    {
        out->binding_state = BLE_LINK_BINDING_UNBOUND;
    }
    if (facts->authorized)
    {
        out->authorization_state = BLE_LINK_AUTHORIZATION_AUTHORIZED;
    }
    else if (facts->session_authenticated)
    {
        out->authorization_state =
            BLE_LINK_AUTHORIZATION_BOOTSTRAP_AUTHENTICATED;
    }
    else
    {
        out->authorization_state = BLE_LINK_AUTHORIZATION_UNAUTHORIZED;
    }
    out->encrypted = facts->encrypted;
    out->secure_connections_bond_verified =
        facts->secure_connections_bond_verified;
    out->identity_known = facts->identity_known;
}

static void _ble_link_service_encode_link_state(
    uint8_t *out, size_t *pos, const ble_link_state_snapshot_t *link_state)
{
    _ble_link_service_write_tag(out, pos, 1U, 1U);
    _ble_link_service_write_fixed64(out, pos, link_state->boot_id);
    if (link_state->binding_state != BLE_LINK_BINDING_UNSPECIFIED)
    {
        _ble_link_service_write_tag(out, pos, 2U, 0U);
        _ble_link_service_write_varint(out, pos, link_state->binding_state);
    }
    if (link_state->authorization_state !=
            BLE_LINK_AUTHORIZATION_UNSPECIFIED)
    {
        _ble_link_service_write_tag(out, pos, 3U, 0U);
        _ble_link_service_write_varint(out, pos,
                                       link_state->authorization_state);
    }
    if (link_state->encrypted)
    {
        _ble_link_service_write_tag(out, pos, 4U, 0U);
        _ble_link_service_write_varint(out, pos, 1U);
    }
    if (link_state->secure_connections_bond_verified)
    {
        _ble_link_service_write_tag(out, pos, 5U, 0U);
        _ble_link_service_write_varint(out, pos, 1U);
    }
    if (link_state->identity_known)
    {
        _ble_link_service_write_tag(out, pos, 6U, 0U);
        _ble_link_service_write_varint(out, pos, 1U);
    }
}

/**
 * @brief Encode a Snapshot message (event_sequence + LinkState).
 */
static size_t _ble_link_service_encode_snapshot(
    uint8_t *out, size_t capacity, uint64_t event_sequence,
    const ble_link_state_snapshot_t *link_state)
{
    uint8_t link_state_bytes[64];
    size_t link_state_len = 0U;

    _ble_link_service_encode_link_state(link_state_bytes, &link_state_len,
                                        link_state);
    const size_t size = 1U + 8U + 1U +
                        _ble_link_service_varint_size(link_state_len) +
                        link_state_len;

    if (capacity < size)
    {
        return 0U;
    }
    size_t pos = 0U;

    _ble_link_service_write_tag(out, &pos, 1U, 1U);
    _ble_link_service_write_fixed64(out, &pos, event_sequence);
    _ble_link_service_write_tag(out, &pos, 2U, 2U);
    _ble_link_service_write_bytes(out, &pos, link_state_bytes,
                                  link_state_len);
    return size;
}

static void _ble_link_service_encode_capabilities(uint8_t *out, size_t *pos)
{
    /* protocol_version {major=1} */
    static const uint8_t protocol_version[] = {0x08, 0x01};
    /* profile_version {major=1} */
    static const uint8_t profile_version[] = {0x08, 0x01};
    /* security {sc_only, key=16, max_bonds=1, protocomm 2, patch 1,
     *          local_confirmation, application_credential} */
    static const uint8_t security[] =
    {
        0x08, 0x01, 0x10, 0x10, 0x18, 0x01, 0x20, 0x02,
        0x28, 0x01, 0x30, 0x01, 0x38, 0x01,
    };
    /* framing {framing_version=1, header_bytes=8, preferred_att_mtu=498,
     *          max_control=4096, max_session=1024} */
    uint8_t framing[32];
    size_t framing_pos = 0U;

    _ble_link_service_write_tag(framing, &framing_pos, 1U, 0U);
    _ble_link_service_write_varint(framing, &framing_pos, 1U);
    _ble_link_service_write_tag(framing, &framing_pos, 2U, 0U);
    _ble_link_service_write_varint(framing, &framing_pos,
                                   BLE_LINK_FRAMING_HEADER_BYTES);
    _ble_link_service_write_tag(framing, &framing_pos, 3U, 0U);
    _ble_link_service_write_varint(framing, &framing_pos,
                                   BLE_LINK_SERVICE_PREFERRED_ATT_MTU);
    _ble_link_service_write_tag(framing, &framing_pos, 4U, 0U);
    _ble_link_service_write_varint(framing, &framing_pos,
                                   BLE_LINK_SERVICE_CONTROL_MAX_BYTES);
    _ble_link_service_write_tag(framing, &framing_pos, 5U, 0U);
    _ble_link_service_write_varint(framing, &framing_pos,
                                   BLE_LINK_SERVICE_SESSION_MAX_BYTES);

    _ble_link_service_write_tag(out, pos, 1U, 2U);
    _ble_link_service_write_bytes(out, pos, protocol_version,
                                  sizeof(protocol_version));
    _ble_link_service_write_tag(out, pos, 2U, 2U);
    _ble_link_service_write_bytes(out, pos, profile_version,
                                  sizeof(profile_version));
    _ble_link_service_write_tag(out, pos, 3U, 2U);
    _ble_link_service_write_bytes(out, pos, framing, framing_pos);
    _ble_link_service_write_tag(out, pos, 4U, 2U);
    _ble_link_service_write_bytes(out, pos, security, sizeof(security));
    /* No features are advertised until they are implemented. */
}

static void _ble_link_service_encode_authorize_prepare(
    uint8_t *out, size_t *pos)
{
    _ble_link_service_write_tag(out, pos, 1U, 1U);
    _ble_link_service_write_fixed64(out, pos,
                                    s_service.auth_txn.authorization_txn_id);
    _ble_link_service_write_tag(out, pos, 2U, 2U);
    _ble_link_service_write_bytes(out, pos, s_service.auth_txn.credential_id,
                                  BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
    /* application_password=3: fake fixed password. */
    static const uint8_t password[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES] =
    {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    };

    _ble_link_service_write_tag(out, pos, 3U, 2U);
    _ble_link_service_write_bytes(out, pos, password, sizeof(password));
    _ble_link_service_write_tag(out, pos, 4U, 0U);
    _ble_link_service_write_varint(out, pos,
                                   BLE_LINK_SERVICE_AUTH_EXPIRES_MS);
}

static void _ble_link_service_encode_authorization_result(
    uint8_t *out, size_t *pos)
{
    static const uint8_t device_auth_id[BLE_LINK_SERVICE_DEVICE_AUTH_ID_BYTES] =
    {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    };

    _ble_link_service_write_tag(out, pos, 1U, 2U);
    _ble_link_service_write_bytes(out, pos, s_service.auth_txn.credential_id,
                                  BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
    _ble_link_service_write_tag(out, pos, 2U, 2U);
    _ble_link_service_write_bytes(out, pos, device_auth_id,
                                  sizeof(device_auth_id));
    _ble_link_service_write_tag(out, pos, 3U, 0U);
    _ble_link_service_write_varint(out, pos,
                                   BLE_LINK_AUTHORIZATION_AUTHORIZED);
}

static void _ble_link_service_encode_event_subscription(
    uint8_t *out, size_t *pos, const ble_link_service_facts_t *facts)
{
    ble_link_state_snapshot_t link_state;

    _ble_link_service_build_link_state(facts, &link_state);
    uint8_t snapshot_bytes[64];
    const size_t snapshot_len = _ble_link_service_encode_snapshot(
                                    snapshot_bytes, sizeof(snapshot_bytes),
                                    s_service.subscriber.sequence_baseline,
                                    &link_state);

    _ble_link_service_write_tag(out, pos, 1U, 2U);
    _ble_link_service_write_bytes(out, pos, s_service.subscriber.event_key,
                                  BLE_LINK_SERVICE_EVENT_KEY_BYTES);
    _ble_link_service_write_tag(out, pos, 2U, 2U);
    _ble_link_service_write_bytes(out, pos,
                                  s_service.subscriber.nonce_prefix,
                                  BLE_LINK_SERVICE_NONCE_PREFIX_BYTES);
    _ble_link_service_write_tag(out, pos, 3U, 5U);
    _ble_link_service_write_fixed32(out, pos,
                                    s_service.subscriber.subscription_id);
    _ble_link_service_write_tag(out, pos, 4U, 1U);
    _ble_link_service_write_fixed64(out, pos,
                                    s_service.subscriber.sequence_baseline);
    _ble_link_service_write_tag(out, pos, 5U, 2U);
    _ble_link_service_write_bytes(out, pos, snapshot_bytes, snapshot_len);
}

/**
 * @brief Emit one message payload split into fragments for the negotiated
 * ATT MTU.
 */
static bool _ble_link_service_emit_fragments(
    const uint8_t *payload, size_t payload_len, uint32_t att_mtu,
    ble_link_service_tx_channel_t channel)
{
    /* The fragment buffer only needs to hold one ATT value plus the
     * framing header; the source message lives elsewhere. */
    uint8_t fragment[BLE_LINK_FRAMING_HEADER_BYTES +
                     BLE_LINK_SERVICE_PREFERRED_ATT_MTU];
    size_t max_payload = 0U;

    if (att_mtu >= BLE_LINK_FRAMING_HEADER_BYTES + 3U)
    {
        max_payload = att_mtu - 3U - BLE_LINK_FRAMING_HEADER_BYTES;
    }
    /* The chunk must fit the local fragment array. */
    if (max_payload > sizeof(fragment) - BLE_LINK_FRAMING_HEADER_BYTES)
    {
        max_payload = sizeof(fragment) - BLE_LINK_FRAMING_HEADER_BYTES;
    }
    if (max_payload == 0U)
    {
        return false;
    }
    const size_t fragment_count =
        (payload_len + max_payload - 1U) / max_payload;

    if (fragment_count > s_service.max_pending_frames)
    {
        /* The TX queue cannot hold this response: fail closed. */
        return false;
    }
    size_t offset = 0U;
    uint16_t frame_id;

    s_service.outbound_frame_id++;
    if (s_service.outbound_frame_id == 0U)
    {
        s_service.outbound_frame_id = 1U;
    }
    frame_id = s_service.outbound_frame_id;
    while (offset < payload_len)
    {
        const size_t remaining = payload_len - offset;
        const size_t chunk = (remaining > max_payload) ?
                             max_payload : remaining;
        uint8_t flags = 0U;

        if (offset == 0U)
        {
            flags |= BLE_LINK_FRAMING_FLAG_START;
        }
        if (offset + chunk == payload_len)
        {
            flags |= BLE_LINK_FRAMING_FLAG_END;
        }
        fragment[0] = 1U;
        fragment[1] = flags;
        fragment[2] = (uint8_t)(frame_id & 0xffU);
        fragment[3] = (uint8_t)(frame_id >> 8U);
        fragment[4] = (uint8_t)(payload_len & 0xffU);
        fragment[5] = (uint8_t)((payload_len >> 8U) & 0xffU);
        fragment[6] = (uint8_t)(offset & 0xffU);
        fragment[7] = (uint8_t)((offset >> 8U) & 0xffU);
        memcpy(&fragment[BLE_LINK_FRAMING_HEADER_BYTES], &payload[offset],
               chunk);
        const bool is_last = (offset + chunk == payload_len);

        if (s_service.output(fragment, BLE_LINK_FRAMING_HEADER_BYTES + chunk,
                             channel, is_last, s_service.output_arg) != ESP_OK)
        {
            return false;
        }
        offset += chunk;
    }
    return true;
}

/**
 * @brief Build and emit one response envelope.
 */
static void _ble_link_service_emit_response(
    uint64_t request_id, uint32_t error, ble_link_codec_response_tag_t body_tag,
    const uint8_t *body, size_t body_len, uint32_t att_mtu,
    ble_link_service_tx_channel_t channel)
{
    uint8_t response_bytes[512];
    size_t response_len = 0U;
    ble_link_codec_response_t response;

    memset(&response, 0, sizeof(response));
    response.request_id = request_id;
    response.error = error;
    response.body = body_tag;
    response.body_data = body;
    response.body_len = body_len;
    if (ble_link_codec_encode_response(&response, response_bytes,
                                       sizeof(response_bytes),
                                       &response_len) != ESP_OK)
    {
        return;
    }
    uint8_t envelope_bytes[640];
    size_t envelope_len = 0U;
    ble_link_codec_envelope_t envelope;

    memset(&envelope, 0, sizeof(envelope));
    envelope.protocol_major = BLE_LINK_SERVICE_PROTOCOL_MAJOR;
    envelope.boot_id = s_service.boot_id;
    envelope.body = BLE_LINK_CODEC_BODY_RESPONSE;
    envelope.body_data = response_bytes;
    envelope.body_len = response_len;
    if (ble_link_codec_encode_envelope(&envelope, envelope_bytes,
                                       sizeof(envelope_bytes),
                                       &envelope_len) != ESP_OK)
    {
        return;
    }
    s_service.pending_transactions++;
    if (!_ble_link_service_emit_fragments(envelope_bytes, envelope_len,
                                          att_mtu, channel))
    {
        /* A rejected fragment aborts the outbound transaction and closes
         * the associated session per the framing contract. */
        s_service.pending_transactions = 0U;
        ble_link_session_security2_close_current(
            s_service.current_facts.connection_generation);
    }
}

/**
 * @brief Response channel for the current RX channel.
 */
static ble_link_service_tx_channel_t _ble_link_service_response_channel(void)
{
    return (s_service.current_channel == BLE_LINK_SERVICE_RX_SESSION) ?
           BLE_LINK_SERVICE_TX_SESSION : BLE_LINK_SERVICE_TX_CONTROL_RESPONSE;
}

static uint32_t _ble_link_service_handle_capabilities(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)facts;
    (void)arg;
    uint8_t body[128];
    size_t body_len = 0U;

    _ble_link_service_encode_capabilities(body, &body_len);
    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_CAPABILITIES, body, body_len,

        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}

static uint32_t _ble_link_service_handle_snapshot(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)facts;
    (void)arg;
    ble_link_state_snapshot_t link_state;
    uint8_t body[64];

    _ble_link_service_build_link_state(&s_service.current_facts,
                                       &link_state);
    const size_t body_len = _ble_link_service_encode_snapshot(
                                body, sizeof(body), ble_link_events_baseline(),
                                &link_state);

    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_SNAPSHOT, body, body_len,

        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}

static uint32_t _ble_link_service_handle_authorize_prepare(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)facts;
    (void)arg;
    if (!s_service.authorize_enabled)
    {
        return BLE_LINK_ERROR_UNSUPPORTED_OPERATION;
    }
    if (s_service.auth_txn.active)
    {
        return BLE_LINK_ERROR_CONFLICT;
    }
    s_service.auth_txn.active = true;
    s_service.auth_txn.committed = false;
    s_service.auth_txn.authorization_txn_id = 723685415333072913ULL;
    for (size_t i = 0U; i < BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES; ++i)
    {
        s_service.auth_txn.credential_id[i] = (uint8_t)(i + 1U);
    }
    uint8_t body[64];
    size_t body_len = 0U;

    _ble_link_service_encode_authorize_prepare(body, &body_len);
    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_AUTHORIZE_PREPARE, body, body_len,

        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}

/**
 * @brief Parse an AuthorizeCommitRequest body with strict bounds.
 *
 * Fields: 1 fixed64 authorization_txn_id, 2 bytes credential_id. The whole
 * message must parse and the credential length must be exactly
 * BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES.
 */
static bool _ble_link_service_parse_authorize_commit(
    const uint8_t *body, size_t body_len, uint64_t *out_txn_id,
    uint8_t *out_credential, size_t *out_credential_len)
{
    uint64_t txn_id = 0U;
    uint8_t credential[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
    size_t credential_len = 0U;
    size_t pos = 0U;
    bool saw_txn = false;
    bool saw_credential = false;

    while (pos < body_len)
    {
        const uint64_t tag = body[pos];

        if (tag >= 0x80U)
        {
            return false;
        }
        pos++;
        const uint64_t field = tag >> 3U;
        const uint64_t wire = tag & 7U;

        if (field == 1U && wire == 1U) /* fixed64 */
        {
            if (body_len - pos < 8U)
            {
                return false;
            }
            uint64_t value = 0U;

            for (unsigned int i = 0U; i < 8U; ++i)
            {
                value |= (uint64_t)body[pos + i] << (8U * i);
            }
            pos += 8U;
            txn_id = value;
            saw_txn = true;
        }
        else if (field == 2U && wire == 2U) /* length-delimited */
        {
            if (pos >= body_len || body[pos] >= 0x80U)
            {
                return false;
            }
            const size_t len = body[pos];

            pos++;
            if (body_len - pos < len)
            {
                return false;
            }
            if (len == BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES)
            {
                memcpy(credential, &body[pos], len);
                credential_len = len;
                saw_credential = true;
            }
            pos += len;
        }
        else
        {
            return false;
        }
    }
    if (!saw_txn || !saw_credential)
    {
        return false;
    }
    *out_txn_id = txn_id;
    memcpy(out_credential, credential, credential_len);
    *out_credential_len = credential_len;
    return true;
}

static uint32_t _ble_link_service_handle_authorize_commit(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)arg;
    if (!s_service.authorize_enabled)
    {
        return BLE_LINK_ERROR_UNSUPPORTED_OPERATION;
    }
    uint64_t txn_id = 0U;
    uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
    size_t credential_len = 0U;
    bool ok = false;

    if (s_service.auth_txn.active)
    {
        if (_ble_link_service_parse_authorize_commit(
                    request->body_data, request->body_len, &txn_id,
                    credential_id, &credential_len))
        {
            ok = (txn_id == s_service.auth_txn.authorization_txn_id &&
                  credential_len == BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES &&
                  memcmp(credential_id, s_service.auth_txn.credential_id,
                         credential_len) == 0);
            if (ok && s_service.auth_txn.committed)
            {
                /* Idempotent replay of a committed transaction. */
                uint8_t replay[64];
                size_t replay_len = 0U;

                _ble_link_service_encode_authorization_result(
                    replay, &replay_len);
                _ble_link_service_emit_response(
                    request->request_id, BLE_LINK_ERROR_OK,
                    BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT, replay,
                    replay_len,

                    s_service.current_facts.preferred_att_mtu,
                    _ble_link_service_response_channel());
                return BLE_LINK_ERROR_OK;
            }
        }
    }
    if (!ok)
    {
        s_service.auth_txn.active = false;
        _ble_link_service_emit_response(
            request->request_id, BLE_LINK_ERROR_INVALID_ARGUMENT,
            BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,

            s_service.current_facts.preferred_att_mtu,
            _ble_link_service_response_channel());
        return BLE_LINK_ERROR_OK;
    }
    s_service.auth_txn.committed = true;
    if (ble_link_session_set_authorization(true, 1U) != ESP_OK ||
            ble_link_session_report_session_match_current(
                facts->connection_generation, 1U) != ESP_OK)
    {
        return BLE_LINK_ERROR_INTERNAL;
    }
    uint8_t body_bytes[64];
    size_t body_len_bytes = 0U;

    _ble_link_service_encode_authorization_result(body_bytes,
            &body_len_bytes);
    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT, body_bytes,
        body_len_bytes,
        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}

static uint32_t _ble_link_service_handle_subscribe_events(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)facts;
    (void)arg;
    if (!s_service.authorize_enabled)
    {
        return BLE_LINK_ERROR_UNSUPPORTED_OPERATION;
    }
    if (s_service.subscriber.active)
    {
        return BLE_LINK_ERROR_CONFLICT;
    }
    s_service.subscriber.active = true;
    s_service.subscriber.generation = facts->connection_generation;
    s_service.subscriber.subscription_id = s_service.next_subscription_id;
    s_service.next_subscription_id++;
    s_service.subscriber.sequence_baseline = ble_link_events_baseline();
    for (size_t i = 0U; i < BLE_LINK_SERVICE_EVENT_KEY_BYTES; ++i)
    {
        s_service.subscriber.event_key[i] = 0xabU;
    }
    for (size_t i = 0U; i < BLE_LINK_SERVICE_NONCE_PREFIX_BYTES; ++i)
    {
        s_service.subscriber.nonce_prefix[i] = (uint8_t)(0x10U + i);
    }
    uint8_t body[128];
    size_t body_len = 0U;

    _ble_link_service_encode_event_subscription(
        body, &body_len, &s_service.current_facts);
    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_EVENT_SUBSCRIPTION, body, body_len,

        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}

void ble_link_service_init(
    uint64_t boot_id, ble_link_service_output_t output, void *arg,
    bool authorize_enabled, size_t max_pending_frames)
{
    memset(&s_service, 0, sizeof(s_service));
    s_service.boot_id = boot_id;
    s_service.output = output;
    s_service.output_arg = arg;
    s_service.next_subscription_id = 1U;
    s_service.authorize_enabled = authorize_enabled;
    s_service.max_pending_frames = (max_pending_frames > 0U) ?
                                   max_pending_frames : 1U;
    ble_link_reassembler_init(&s_service.reassembler[0],
                              s_service.reassembly_buffer[0],
                              BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES);
    ble_link_reassembler_init(&s_service.reassembler[1],
                              s_service.reassembly_buffer[1],
                              BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES);
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_GET_CAPABILITIES,
        _ble_link_service_handle_capabilities, NULL);
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_GET_LINK_SNAPSHOT,
        _ble_link_service_handle_snapshot, NULL);
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE,
        _ble_link_service_handle_authorize_prepare, NULL);
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_AUTHORIZE_COMMIT,
        _ble_link_service_handle_authorize_commit, NULL);
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_SUBSCRIBE_EVENTS,
        _ble_link_service_handle_subscribe_events, NULL);
}

void ble_link_service_reset(void)
{
    ble_link_dispatcher_reset();
    memset(&s_service, 0, sizeof(s_service));
}

bool ble_link_service_has_partial_frame(
    ble_link_service_rx_channel_t channel)
{
    if (channel != BLE_LINK_SERVICE_RX_SESSION &&
            channel != BLE_LINK_SERVICE_RX_CONTROL)
    {
        return false;
    }
    return s_service.reassembler[channel].started;
}

void ble_link_service_idle_timeout(uint32_t generation)
{
    /* close_current validates the generation; a stale timeout has no
     * effect. */
    _ble_link_service_abort_session(generation);
}

esp_err_t ble_link_service_feed(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const uint8_t *value, size_t len)
{
    if (facts == NULL || value == NULL || s_service.output == NULL ||
            (channel != BLE_LINK_SERVICE_RX_SESSION &&
             channel != BLE_LINK_SERVICE_RX_CONTROL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (facts->connection_generation != s_service.feed_generation)
    {
        if (facts->connection_generation < s_service.feed_generation)
        {
            /* A stale feed from a retired generation has no effect. */
            return ESP_OK;
        }
        /* A new connection generation discards the partial frame, the
         * subscription, the transaction, and the session ids. The Security
         * 2 session close is owned by the caller's disconnect handling. */
        ble_link_reassembler_reset(&s_service.reassembler[0]);
        ble_link_reassembler_reset(&s_service.reassembler[1]);
        s_service.subscriber.active = false;
        s_service.auth_txn.active = false;
        s_service.auth_txn.committed = false;
        s_service.pending_transactions = 0U;
        ble_link_dispatcher_clear_session();
        s_service.feed_generation = facts->connection_generation;
    }
    ble_link_reassembler_t *slot = &s_service.reassembler[channel];
    const size_t slot_capacity =
        (channel == BLE_LINK_SERVICE_RX_SESSION) ?
        BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES :
        BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES;
    ble_link_fragment_t fragment;

    if (ble_link_reassembler_parse(value, len, &fragment) != ESP_OK)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t total_length = fragment.total_length;

    esp_err_t result = ble_link_reassembler_accept(slot, &fragment);

    if (result == ESP_ERR_NOT_FINISHED)
    {
        return ESP_ERR_NOT_FINISHED;
    }
    if (result != ESP_OK)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return result;
    }
    if (total_length > slot_capacity)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_NO_MEM;
    }
    ble_link_codec_envelope_t envelope;
    ble_link_codec_request_t request;

    if (ble_link_codec_decode_envelope(
                slot->buffer, total_length, &envelope) != ESP_OK)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_INVALID_STATE;
    }
    /* A boot id mismatch is terminal regardless of the transaction gate
     * or the envelope body: the session is closed without a response. */
    if (envelope.boot_id != facts->active_boot_id)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_INVALID_STATE;
    }
    if (envelope.body != BLE_LINK_CODEC_BODY_REQUEST ||
            ble_link_codec_decode_request(
                envelope.body_data, envelope.body_len, &request) != ESP_OK)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.pending_transactions > 0U)
    {
        /* One transaction at a time: the previous response must confirm
         * before a new request is admitted. */
        uint32_t busy_error = BLE_LINK_ERROR_BUSY;

        _ble_link_service_emit_response(
            request.request_id, busy_error,
            BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,
            facts->preferred_att_mtu,
            (channel == BLE_LINK_SERVICE_RX_SESSION) ?
            BLE_LINK_SERVICE_TX_SESSION : BLE_LINK_SERVICE_TX_CONTROL_RESPONSE);
        return ESP_OK;
    }
    /* The bootstrap authorize flow is reachable before authorization and
     * arrives on the session channel; every other request is a control
     * request. A request on the wrong channel is rejected. */
    const bool bootstrap =
        (request.body == BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE ||
         request.body == BLE_LINK_CODEC_REQUEST_AUTHORIZE_COMMIT);

    if ((bootstrap && channel != BLE_LINK_SERVICE_RX_SESSION) ||
            (!bootstrap && channel != BLE_LINK_SERVICE_RX_CONTROL))
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t admission_error = 0U;
    const ble_link_session_channel_t admission_channel =
        bootstrap ? BLE_LINK_SESSION_CHANNEL_SESSION :
        BLE_LINK_SESSION_CHANNEL_CONTROL;

    if (ble_link_session_query_admission(
                facts->connection_generation, admission_channel,
                &admission_error) != ESP_OK ||
            admission_error != BLE_LINK_ERROR_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_service.current_facts = *facts;
    s_service.current_channel = channel;
    ble_link_dispatcher_facts_t dispatcher_facts;

    memset(&dispatcher_facts, 0, sizeof(dispatcher_facts));
    dispatcher_facts.active_boot_id = facts->active_boot_id;
    dispatcher_facts.connection_generation = facts->connection_generation;
    dispatcher_facts.encrypted = facts->encrypted;
    dispatcher_facts.session_authenticated = facts->session_authenticated;
    dispatcher_facts.authorized = facts->authorized;
    uint32_t dispatch_error = 0U;

    if (ble_link_dispatcher_handle_request(
                &envelope, &request, &dispatcher_facts, &dispatch_error) != ESP_OK)
    {
        if (dispatch_error != 0U)
        {
            _ble_link_service_emit_response(
                request.request_id, dispatch_error,
                BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,

                s_service.current_facts.preferred_att_mtu,
                _ble_link_service_response_channel());
        }
        return ESP_ERR_NO_MEM;
    }
    if (dispatch_error != BLE_LINK_ERROR_OK)
    {
        /* Encode the stable LinkError as a body-less response. A boot id
         * mismatch is terminal per the lifecycle contract: the session is
         * closed, not merely answered. */
        if (dispatch_error == BLE_LINK_ERROR_UNAVAILABLE &&
                envelope.boot_id != facts->active_boot_id)
        {
            _ble_link_service_abort_session(facts->connection_generation);
            return ESP_OK;
        }
        _ble_link_service_emit_response(
            request.request_id, dispatch_error,
            BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,

            s_service.current_facts.preferred_att_mtu,
            _ble_link_service_response_channel());
    }
    return ESP_OK;
}

esp_err_t ble_link_service_publish_link_state(
    const ble_link_service_facts_t *facts,
    const ble_link_state_snapshot_t *link_state)
{
    if (facts == NULL || link_state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_service.subscriber.active)
    {
        return ESP_OK;
    }
    /* A publish from a retired generation has no effect. */
    if (facts->connection_generation != s_service.subscriber.generation)
    {
        return ESP_OK;
    }
    /* Event publication requires current-generation authorized admission. */
    uint32_t admission_error = 0U;

    if (ble_link_session_query_admission(
                facts->connection_generation,
                BLE_LINK_SESSION_CHANNEL_EVENT, &admission_error) != ESP_OK ||
            admission_error != BLE_LINK_ERROR_OK)
    {
        s_service.subscriber.active = false;
        return ESP_OK;
    }
    const uint64_t sequence = ble_link_events_next();

    if (sequence == 0U)
    {
        return ESP_OK;
    }
    /* Event { sequence=1; link_state_changed=10 { link_state=1 {...} } } */
    uint8_t event_body[192];
    size_t event_len = 0U;
    uint8_t changed_body[64];
    size_t changed_len = 0U;

    _ble_link_service_encode_link_state(changed_body, &changed_len,
                                        link_state);
    uint8_t changed_msg[64];
    size_t changed_msg_len = 0U;

    _ble_link_service_write_tag(changed_msg, &changed_msg_len, 1U, 2U);
    _ble_link_service_write_varint(changed_msg, &changed_msg_len,
                                   changed_len);
    memcpy(&changed_msg[changed_msg_len], changed_body, changed_len);
    changed_msg_len += changed_len;
    _ble_link_service_write_tag(event_body, &event_len, 1U, 1U);
    _ble_link_service_write_fixed64(event_body, &event_len, sequence);
    _ble_link_service_write_tag(event_body, &event_len, 10U, 2U);
    _ble_link_service_write_bytes(event_body, &event_len, changed_msg,
                                  changed_msg_len);
    uint8_t envelope_bytes[512];
    size_t envelope_len = 0U;
    ble_link_codec_envelope_t envelope;

    memset(&envelope, 0, sizeof(envelope));
    envelope.protocol_major = BLE_LINK_SERVICE_PROTOCOL_MAJOR;
    envelope.boot_id = s_service.boot_id;
    envelope.body = BLE_LINK_CODEC_BODY_EVENT;
    envelope.body_data = event_body;
    envelope.body_len = event_len;
    if (ble_link_codec_encode_envelope(&envelope, envelope_bytes,
                                       sizeof(envelope_bytes),
                                       &envelope_len) != ESP_OK)
    {
        return ESP_ERR_NO_MEM;
    }
    s_service.pending_transactions++;
    if (!_ble_link_service_emit_fragments(
                envelope_bytes, envelope_len,
                s_service.current_facts.preferred_att_mtu,
                BLE_LINK_SERVICE_TX_CONTROL_EVENT))
    {
        s_service.pending_transactions = 0U;
        ble_link_session_security2_close_current(
            facts->connection_generation);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
