#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_gatt_registry.h"
#include "ble_link_events.h"
#include "ble_link_gatt.h"
#include "ble_link_service.h"
#include "ble_link_session.h"
#include "ble_link_state.h"
#include "ble_tx_scheduler.h"

#define DBG_TAG "ble_link_gatt"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

/* NimBLE ATT error codes; the host build provides the canonical constants
 * through host/ble_att.h, the host tests define the ones we emit. */
#ifndef BLE_ATT_ERR_INSUFFICIENT_AUTHEN
    #define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 0x0f
#endif
#ifndef BLE_ATT_ERR_INSUFFICIENT_RES
    #define BLE_ATT_ERR_INSUFFICIENT_RES 0x13
#endif

/* Device Link profile service UUID: 3e203192-b4bb-4e59-a28a-3d1157854ea3. */
static const uint8_t s_device_link_uuid[16] =
{
    0xa3, 0x4e, 0x85, 0x57, 0x11, 0x3d, 0x8a, 0xa2,
    0x59, 0x4e, 0xbb, 0xb4, 0x92, 0x31, 0x20, 0x3e,
};

/* link_state: a4781f24-dc6b-44eb-864f-23a8a4ef82b5. */
static const uint8_t s_link_state_uuid[16] =
{
    0xb5, 0x82, 0xef, 0xa4, 0x8a, 0x23, 0x4f, 0x86,
    0xeb, 0x44, 0x6b, 0xdc, 0x24, 0x1f, 0x78, 0xa4,
};

/* session_rx: 4801e643-cb4c-4dd8-b85c-e6e0fccdf0a2. */
static const uint8_t s_session_rx_uuid[16] =
{
    0xa2, 0xf0, 0xcd, 0xfc, 0xe0, 0xe6, 0x5c, 0xb8,
    0xd8, 0x4d, 0xcb, 0x4c, 0x43, 0xe6, 0x01, 0x48,
};

/* session_tx: 2be85710-beac-402c-83a1-ec5fd2af052a. */
static const uint8_t s_session_tx_uuid[16] =
{
    0x2a, 0x05, 0xaf, 0xd2, 0x5f, 0xec, 0xa1, 0x83,
    0x2c, 0x40, 0xac, 0xbe, 0x10, 0x57, 0xe8, 0x2b,
};

/* control_rx: 81a14662-669d-4772-8e0c-fb3d403d13c8. */
static const uint8_t s_control_rx_uuid[16] =
{
    0xc8, 0x13, 0x3d, 0x40, 0xfb, 0x3d, 0x0c, 0x8e,
    0x72, 0x47, 0x9d, 0x66, 0x62, 0x46, 0xa1, 0x81,
};

/* control_tx: 11735ac7-1e40-4a9c-b562-b8f64c03883a. */
static const uint8_t s_control_tx_uuid[16] =
{
    0x3a, 0x88, 0x03, 0x4c, 0xf6, 0xb8, 0x62, 0xb5,
    0x9c, 0x4a, 0x40, 0x1e, 0xc7, 0x5a, 0x73, 0x11,
};

/* transfer service: 2b837278-c4ef-426e-9fa0-4e32880ac31c. */
static const uint8_t s_transfer_service_uuid[16] =
{
    0x1c, 0x3a, 0xc8, 0x80, 0x32, 0x4e, 0xa0, 0x9f,
    0x6e, 0x42, 0xef, 0xc4, 0x78, 0x72, 0x83, 0x2b,
};

/* transfer_rx: f3598d4d-e9ff-471e-97ac-76728628f0a1. */
static const uint8_t s_transfer_rx_uuid[16] =
{
    0xa1, 0xf0, 0x28, 0x86, 0x72, 0x76, 0xac, 0x97,
    0x1e, 0x47, 0xff, 0xe9, 0x4d, 0x8d, 0x59, 0xf3,
};

/* transfer_tx: a99aa0f1-7dd0-46de-a17d-a619efb2c595. */
static const uint8_t s_transfer_tx_uuid[16] =
{
    0x95, 0xc5, 0xb2, 0xef, 0x19, 0xa6, 0x7d, 0xa1,
    0xde, 0x46, 0xdd, 0x70, 0xf1, 0xa0, 0x9a, 0xa9,
};

/* transfer_state: ec4125f1-16d5-4a02-96ba-b7efb894d49b. */
static const uint8_t s_transfer_state_uuid[16] =
{
    0x9b, 0x4d, 0x94, 0xb8, 0xef, 0xb7, 0xba, 0x96,
    0x02, 0x4a, 0xd5, 0x16, 0xf1, 0x25, 0x41, 0xec,
};

typedef struct ble_link_gatt
{
    const ble_link_gatt_config_t *config;
    bool registered;
    uint16_t handles[5];
    uint8_t last_link_state[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    size_t last_link_state_len;
} ble_link_gatt_t;

static ble_link_gatt_t s_gatt;
static ble_link_work_submit_fn s_work_submit;
static void *s_work_submit_arg;

void ble_link_gatt_set_work_submit(ble_link_work_submit_fn submit, void *arg)
{
    s_work_submit = submit;
    s_work_submit_arg = arg;
}

static int _ble_link_gatt_access(
    uint16_t conn_handle, uint16_t attr_handle,
    ble_gatt_registry_access_context_t *context, void *arg);

static ble_gatt_registry_characteristic_t s_characteristics[5] =
{
    {
        .uuid = s_link_state_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_READ |
        BLE_GATT_REGISTRY_PROP_NOTIFY,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
    },
    {
        .uuid = s_session_rx_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_WRITE,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND,
    },
    {
        .uuid = s_session_tx_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_INDICATE,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND,
    },
    {
        .uuid = s_control_rx_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_WRITE,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
    },
    {
        .uuid = s_control_tx_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_INDICATE |
        BLE_GATT_REGISTRY_PROP_NOTIFY,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
    },
};

static const ble_gatt_registry_service_t s_service =
{
    .uuid = s_device_link_uuid,
    .characteristics = s_characteristics,
    .characteristic_count = 5U,
};

static ble_gatt_registry_characteristic_t s_transfer_characteristics[3] =
{
    {
        .uuid = s_transfer_rx_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_WRITE_NO_RESPONSE,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED_TRANSFER,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED_TRANSFER,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED_TRANSFER,
    },
    {
        .uuid = s_transfer_tx_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_NOTIFY,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED_TRANSFER,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED_TRANSFER,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED_TRANSFER,
    },
    {
        .uuid = s_transfer_state_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_READ |
        BLE_GATT_REGISTRY_PROP_NOTIFY,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
    },
};

static const ble_gatt_registry_service_t s_transfer_service =
{
    .uuid = s_transfer_service_uuid,
    .characteristics = s_transfer_characteristics,
    .characteristic_count = 3U,
};

/**
 * @brief Build the current service facts from the session module.
 */
static esp_err_t _ble_link_gatt_service_facts(
    ble_link_service_facts_t *out)
{
    if (s_gatt.config == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(out, 0, sizeof(*out));
    ble_link_dispatcher_facts_t session_facts;
    bool bond_verified = false;
    bool identity_known = false;

    if (ble_link_session_get_facts(
                s_gatt.config->connection_generation,
                &session_facts) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    (void)ble_link_session_get_security_facts(
        s_gatt.config->connection_generation, &bond_verified,
        &identity_known);
    out->active_boot_id = session_facts.active_boot_id;
    out->connection_generation = session_facts.connection_generation;
    out->preferred_att_mtu = s_gatt.config->att_mtu;
    out->encrypted = session_facts.encrypted;
    out->session_authenticated = session_facts.session_authenticated;
    out->authorized = session_facts.authorized;
    out->identity_known = identity_known;
    out->secure_connections_bond_verified = bond_verified;
    out->peer_addr_type = s_gatt.config->peer_addr_type;
    memcpy(out->peer_addr, s_gatt.config->peer_addr,
           sizeof(out->peer_addr));
    return ESP_OK;
}

/**
 * @brief Output sink from the link service: submit to the TX scheduler.
 */
static esp_err_t _ble_link_gatt_output(
    const uint8_t *value, size_t len,
    ble_link_service_tx_channel_t channel, bool is_last, void *arg)
{
    (void)is_last;
    (void)arg;
    const ble_link_gatt_config_t *config = s_gatt.config;

    if (config == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t handle = 0U;
    ble_tx_scheduler_kind_t kind = BLE_TX_SCHEDULER_KIND_NOTIFY;

    switch (channel)
    {
    case BLE_LINK_SERVICE_TX_SESSION:
        handle = s_gatt.handles[2];
        kind = BLE_TX_SCHEDULER_KIND_INDICATE;
        break;
    case BLE_LINK_SERVICE_TX_CONTROL_RESPONSE:
        handle = s_gatt.handles[4];
        kind = BLE_TX_SCHEDULER_KIND_INDICATE;
        break;
    case BLE_LINK_SERVICE_TX_CONTROL_EVENT:
        handle = s_gatt.handles[4];
        kind = BLE_TX_SCHEDULER_KIND_NOTIFY;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (handle == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return ble_tx_scheduler_submit(kind, config->conn_handle, handle, value,
                                   len, is_last);
}

static int _ble_link_gatt_read_link_state(
    ble_gatt_registry_access_context_t *context)
{
    ble_link_state_t state;
    size_t len = 0U;
    ble_link_service_facts_t facts;

    memset(&state, 0, sizeof(state));
    state.protocol_major = 1U;
    state.profile_major = 1U;
    if (_ble_link_gatt_service_facts(&facts) != ESP_OK)
    {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    state.boot_id = facts.active_boot_id;
    state.state_flags = ble_link_session_get_state_flags();
    if (ble_link_state_encode(&state, context->read_out,
                              context->read_capacity, &len) != ESP_OK)
    {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    *context->read_len = (uint16_t)len;
    return 0;
}

static int _ble_link_gatt_transfer_access(
    uint16_t conn_handle, uint16_t attr_handle,
    ble_gatt_registry_access_context_t *context, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)context;
    (void)arg;
    /* Transfer is not implemented; the service exists and rejects. */
    return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
}

static int _ble_link_gatt_access(
    uint16_t conn_handle, uint16_t attr_handle,
    ble_gatt_registry_access_context_t *context, void *arg)
{
    (void)arg;
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    /* Only the accepted connection may use the link service. */
    if (s_gatt.config == NULL ||
            conn_handle != s_gatt.config->conn_handle)
    {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (ble_gatt_registry_lookup_by_handle(attr_handle, &characteristic) !=
            ESP_OK)
    {
        return -1;
    }
    if (memcmp(characteristic->uuid, s_link_state_uuid, 16U) == 0)
    {
        if (context->op == BLE_GATT_REGISTRY_OP_READ_CHR)
        {
            return _ble_link_gatt_read_link_state(context);
        }
        return -1;
    }
    if (context->op != BLE_GATT_REGISTRY_OP_WRITE_CHR ||
            context->write_data == NULL)
    {
        return -1;
    }
    ble_link_service_facts_t facts;

    if (_ble_link_gatt_service_facts(&facts) != ESP_OK)
    {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    const ble_link_service_rx_channel_t channel =
        (memcmp(characteristic->uuid, s_session_rx_uuid, 16U) == 0) ?
        BLE_LINK_SERVICE_RX_SESSION : BLE_LINK_SERVICE_RX_CONTROL;

    if (channel == BLE_LINK_SERVICE_RX_SESSION &&
            memcmp(characteristic->uuid, s_session_rx_uuid, 16U) != 0)
    {
        return -1;
    }
    /* Admission is checked before any fragment enters the reassembler, so
     * an unauthenticated peer cannot perturb reassembly state. A control
     * write is additionally admitted for an already authenticated long-
     * term session that has not yet restored its authorization: the feed
     * runs the identity-matched authentication transition before the
     * request dispatches. */
    const bool control_channel = channel == BLE_LINK_SERVICE_RX_CONTROL;
    uint32_t admission_error = 0U;

    if (ble_link_session_query_admission(
                facts.connection_generation,
                control_channel ? BLE_LINK_SESSION_CHANNEL_CONTROL :
                BLE_LINK_SESSION_CHANNEL_SESSION,
                &admission_error) != ESP_OK ||
            admission_error != BLE_LINK_ERROR_OK)
    {
        if (!control_channel ||
                s_gatt.config->security_ops == NULL ||
                s_gatt.config->security_ops->is_authenticated == NULL)
        {
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
        /* An already authenticated long-term session, or an established
         * (handshake-started) session that still needs its first
         * protected frame to complete the transition, is admitted; a
         * failed decrypt closes the session. */
        if (!s_gatt.config->security_ops->is_authenticated() &&
                (s_gatt.config->security_ops->session_open == NULL ||
                 !s_gatt.config->security_ops->session_open()))
        {
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
    }
    ble_link_work_t *work = NULL;
    esp_err_t result = ble_link_service_accept(
                           &facts, channel, context->write_data,
                           context->write_len, &work);

    if (result != ESP_OK && result != ESP_ERR_NOT_FINISHED)
    {
        /* Protocol or admission violation: reject the write. */
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (result == ESP_ERR_NOT_FINISHED || work == NULL)
    {
        return 0;
    }
    if (s_work_submit != NULL)
    {
        result = s_work_submit(work, s_work_submit_arg);
        if (result == ESP_OK)
        {
            return 0;
        }
        ble_link_service_release_work(work);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    result = ble_link_service_execute(work);
    ble_link_service_release_work(work);
    return result == ESP_OK ? 0 : BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
}

esp_err_t ble_link_gatt_init(const ble_link_gatt_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_gatt.config = config;
    for (size_t i = 0U; i < 5U; ++i)
    {
        s_characteristics[i].access_cb = _ble_link_gatt_access;
    }
    for (size_t i = 0U; i < 3U; ++i)
    {
        s_transfer_characteristics[i].access_cb =
            _ble_link_gatt_transfer_access;
    }
    ble_link_session_init(config->boot_id);
    const size_t queue_depth = (config->tx_queue_depth > 0U) ?
                               config->tx_queue_depth : 32U;

    ble_link_service_init(config->boot_id, _ble_link_gatt_output,
                          NULL, config->security_ops, queue_depth);
    if (!s_gatt.registered)
    {
        /* The registry is process-global; the services register once. */
        esp_err_t result = ble_gatt_registry_register(&s_service);

        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        {
            return result;
        }
        result = ble_gatt_registry_register(&s_transfer_service);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        {
            return result;
        }
        s_gatt.registered = true;
    }
    return ESP_OK;
}

void ble_link_gatt_reset(void)
{
    ble_link_service_reset();
    const ble_link_gatt_config_t *config = s_gatt.config;

    memset(&s_gatt, 0, sizeof(s_gatt));
    s_gatt.config = config;
}

esp_err_t ble_link_gatt_restart(void)
{
    if (s_gatt.config == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const uint64_t boot_id = s_gatt.config->boot_id;

    for (size_t i = 0U; i < 5U; ++i)
    {
        s_characteristics[i].access_cb = _ble_link_gatt_access;
    }
    for (size_t i = 0U; i < 3U; ++i)
    {
        s_transfer_characteristics[i].access_cb =
            _ble_link_gatt_transfer_access;
    }
    const size_t queue_depth = (s_gatt.config->tx_queue_depth > 0U) ?
                               s_gatt.config->tx_queue_depth : 32U;

    ble_link_service_init(boot_id, _ble_link_gatt_output, NULL,
                          (s_gatt.config != NULL) ?
                          s_gatt.config->security_ops : NULL,
                          queue_depth);
    return ESP_OK;
}

esp_err_t ble_link_gatt_refresh_link_state(void)
{
    if (s_gatt.config == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ble_link_state_t state;
    uint8_t value[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    size_t len = 0U;
    ble_link_service_facts_t facts;

    if (_ble_link_gatt_service_facts(&facts) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&state, 0, sizeof(state));
    state.protocol_major = 1U;
    state.profile_major = 1U;
    state.boot_id = facts.active_boot_id;
    state.state_flags = ble_link_session_get_state_flags();
    if (ble_link_state_encode(&state, value, sizeof(value), &len) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (len != s_gatt.last_link_state_len ||
            memcmp(value, s_gatt.last_link_state, len) != 0)
    {
        memcpy(s_gatt.last_link_state, value, len);
        s_gatt.last_link_state_len = len;
        if (s_gatt.config->publish_link_state != NULL)
        {
            s_gatt.config->publish_link_state(value, len,
                                              s_gatt.config->publish_arg);
        }
    }
    return ESP_OK;
}

void ble_link_gatt_set_connection(
    uint32_t generation, uint16_t conn_handle,
    uint8_t peer_addr_type, const uint8_t peer_addr[6])
{
    if (s_gatt.config != NULL)
    {
        ble_link_gatt_config_t *config = (ble_link_gatt_config_t *)
                                         s_gatt.config;

        config->connection_generation = generation;
        config->conn_handle = conn_handle;
        config->peer_addr_type = peer_addr_type;
        if (peer_addr != NULL)
        {
            memcpy(config->peer_addr, peer_addr, 6U);
        }
    }
}

void ble_link_gatt_update_handles(void)
{
    static const uint8_t *uuids[5] =
    {
        s_link_state_uuid, s_session_rx_uuid, s_session_tx_uuid,
        s_control_rx_uuid, s_control_tx_uuid,
    };

    for (size_t i = 0U; i < 5U; ++i)
    {
        uint16_t handle = 0U;

        if (ble_gatt_registry_get_assigned_handle(uuids[i], &handle) == ESP_OK)
        {
            s_gatt.handles[i] = handle;
        }
    }
}

void ble_link_gatt_on_reassembly_idle_generation(
    uint32_t generation, uint32_t epoch)
{
    if (s_gatt.config != NULL)
    {
        ble_link_service_idle_timeout_epoch(generation, epoch);
    }
}

void ble_link_gatt_set_att_mtu(uint16_t mtu)
{
    if (s_gatt.config != NULL)
    {
        ble_link_gatt_config_t *config = (ble_link_gatt_config_t *)
                                         s_gatt.config;

        /* The Device Link profile caps the negotiated MTU at 498: a peer
         * requesting more is answered with the cap, and every outbound
         * value is bounded by the 498-derived limits. 23 is the mandatory
         * floor. */
        if (mtu > BLE_LINK_GATT_ATT_MTU_MAX)
        {
            mtu = BLE_LINK_GATT_ATT_MTU_MAX;
        }
        config->att_mtu = (mtu >= 23U) ? mtu : 23U;
    }
}


esp_err_t ble_link_gatt_get_att_mtu(uint32_t *out_mtu)
{
    if (out_mtu == NULL || s_gatt.config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_mtu = s_gatt.config->att_mtu;
    return ESP_OK;
}

uint16_t ble_link_gatt_link_state_handle(void)
{
    return s_gatt.handles[0];
}

uint16_t ble_link_gatt_session_tx_handle(void)
{
    return s_gatt.handles[2];
}

uint16_t ble_link_gatt_control_tx_handle(void)
{
    return s_gatt.handles[4];
}

uint16_t ble_link_gatt_session_rx_handle(void)
{
    return s_gatt.handles[1];
}

uint16_t ble_link_gatt_control_rx_handle(void)
{
    return s_gatt.handles[3];
}
