#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ble_gap_manager.h"
#include "ble_gatt_registry.h"
#include "ble_link_gatt.h"
#include "ble_link_service.h"
#include "ble_link_session.h"
#include "ble_tx_scheduler.h"
#include "device_link_v1.h"

#if !defined(UNIT_TEST_HOST)
    #include "host/ble_att.h"
#endif

#define DBG_TAG "ble_link_gatt"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

#ifndef BLE_ATT_ERR_INSUFFICIENT_AUTHEN
    #define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 0x05
#endif

static const uint8_t s_device_link_uuid[16] =
{
    0x31, 0x6a, 0x7b, 0x2f, 0x4c, 0x9c, 0x04, 0x9c,
    0x44, 0x4f, 0xf6, 0x65, 0x10, 0x8c, 0x2a, 0x8f,
};

static const uint8_t s_command_rx_uuid[16] =
{
    0x31, 0x6a, 0x7b, 0x2f, 0x4c, 0x9c, 0x04, 0x9c,
    0x44, 0x4f, 0xf6, 0x65, 0x11, 0x8c, 0x2a, 0x8f,
};

static const uint8_t s_server_tx_uuid[16] =
{
    0x31, 0x6a, 0x7b, 0x2f, 0x4c, 0x9c, 0x04, 0x9c,
    0x44, 0x4f, 0xf6, 0x65, 0x12, 0x8c, 0x2a, 0x8f,
};

typedef struct ble_link_gatt
{
    ble_link_gatt_config_t config;
    bool configured;
    bool registered;
    uint16_t handles[2];
} ble_link_gatt_t;

static ble_link_gatt_t s_gatt;
static ble_link_work_submit_fn s_work_submit;
static void *s_work_submit_arg;
static SemaphoreHandle_t s_gatt_mutex;
static StaticSemaphore_t s_gatt_mutex_control;

static int _ble_link_gatt_access(
    uint16_t conn_handle, uint16_t attr_handle,
    ble_gatt_registry_access_context_t *context, void *arg);

static ble_gatt_registry_characteristic_t s_characteristics[2] =
{
    {
        .uuid = s_command_rx_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_WRITE,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND,
        .access_cb = _ble_link_gatt_access,
    },
    {
        .uuid = s_server_tx_uuid,
        .properties = BLE_GATT_REGISTRY_PROP_INDICATE,
        .read_admission = BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
        .write_admission = BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND,
        .tx_admission = BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND,
        .access_cb = _ble_link_gatt_access,
    },
};

static const ble_gatt_registry_service_t s_service =
{
    .uuid = s_device_link_uuid,
    .characteristics = s_characteristics,
    .characteristic_count = 2U,
};

static void _ble_link_gatt_lock(void)
{
    if (s_gatt_mutex != NULL)
    {
        (void)xSemaphoreTakeRecursive(s_gatt_mutex, portMAX_DELAY);
    }
}

static void _ble_link_gatt_unlock(void)
{
    if (s_gatt_mutex != NULL)
    {
        (void)xSemaphoreGiveRecursive(s_gatt_mutex);
    }
}

void ble_link_gatt_set_work_submit(ble_link_work_submit_fn submit, void *arg)
{
    _ble_link_gatt_lock();
    s_work_submit = submit;
    s_work_submit_arg = arg;
    _ble_link_gatt_unlock();
}

static esp_err_t _ble_link_gatt_service_facts(ble_link_service_facts_t *out)
{
    ble_link_gatt_config_t config;

    _ble_link_gatt_lock();
    if (!s_gatt.configured)
    {
        _ble_link_gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    config = s_gatt.config;
    _ble_link_gatt_unlock();
    memset(out, 0, sizeof(*out));
    ble_link_dispatcher_facts_t session_facts;
    bool bond_verified = false;
    bool identity_known = false;
    bool pairing_window_open = false;

    if (ble_link_session_get_facts(config.connection_generation,
                                   &session_facts) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    (void)ble_link_session_get_security_facts(
        config.connection_generation, &bond_verified, &identity_known);
    (void)ble_link_session_get_connection_pairing_window(
        config.connection_generation, &pairing_window_open);
    out->active_boot_id = session_facts.active_boot_id;
    out->connection_generation = session_facts.connection_generation;
    out->preferred_att_mtu = config.att_mtu;
    out->conn_handle = config.conn_handle;
    out->encrypted = session_facts.encrypted;
    out->session_authenticated = session_facts.session_authenticated;
    out->authorized = session_facts.authorized;
    out->identity_known = identity_known;
    out->secure_connections_bond_verified = bond_verified;
    out->pairing_window_open = pairing_window_open;
    out->peer_addr_type = config.peer_addr_type;
    memcpy(out->peer_addr, config.peer_addr, sizeof(out->peer_addr));
    return ESP_OK;
}

static esp_err_t _ble_link_gatt_output(
    const uint8_t *value, size_t len,
    ble_link_service_tx_channel_t channel, bool is_last, uint32_t flow_id,
    void *arg)
{
    (void)channel;
    (void)arg;
    ble_link_gatt_config_t config;
    uint16_t handle;

    _ble_link_gatt_lock();
    if (!s_gatt.configured)
    {
        _ble_link_gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    config = s_gatt.config;
    handle = s_gatt.handles[1];
    _ble_link_gatt_unlock();
    if (handle == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_gap_manager_is_subscribed_kind(config.conn_handle, handle, false))
    {
        return ESP_ERR_INVALID_STATE;
    }
    const ble_link_operation_identity_t identity =
    {
        .generation = config.connection_generation,
        .flow_id = flow_id,
        .kind = BLE_LINK_OPERATION_TX_INDICATE,
        .conn_handle = config.conn_handle,
    };

    return ble_tx_scheduler_submit(
               BLE_TX_SCHEDULER_KIND_INDICATE, &identity, handle, value, len,
               is_last);
}

static int _ble_link_gatt_access(
    uint16_t conn_handle, uint16_t attr_handle,
    ble_gatt_registry_access_context_t *context, void *arg)
{
    (void)arg;
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    _ble_link_gatt_lock();
    const bool accepted = s_gatt.configured &&
                          conn_handle == s_gatt.config.conn_handle;
    const uint16_t server_tx = s_gatt.handles[1];
    const ble_link_work_submit_fn work_submit = s_work_submit;
    void *work_submit_arg = s_work_submit_arg;
    const uint16_t att_mtu = s_gatt.config.att_mtu;

    _ble_link_gatt_unlock();
    if (!accepted)
    {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (ble_gatt_registry_lookup_by_handle(attr_handle, &characteristic) !=
            ESP_OK)
    {
        return -1;
    }
    if (memcmp(characteristic->uuid, s_command_rx_uuid, 16U) != 0 ||
            context->op != BLE_GATT_REGISTRY_OP_WRITE_CHR ||
            context->write_data == NULL)
    {
        return -1;
    }
    ble_link_service_facts_t facts;

    if (_ble_link_gatt_service_facts(&facts) != ESP_OK)
    {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    device_link_v1_route_input_t input;
    device_link_v1_route_result_t route;

    memset(&input, 0, sizeof(input));
    input.att_mtu = att_mtu;
    input.att_value_length = context->write_len;
    input.encrypted = facts.encrypted;
    input.authenticated = facts.encrypted &&
                          (facts.secure_connections_bond_verified ||
                           facts.session_authenticated);
    input.subscription_enabled = ble_gap_manager_is_subscribed_kind(
                                     conn_handle, server_tx, false);
    input.indication_outstanding = ble_link_service_write_blocked();
    input.value = context->write_data;
    device_link_v1_route_write(&input, &route);
    if (route.kind == DEVICE_LINK_V1_ROUTE_ATT)
    {
        return (int)route.att_error;
    }
    ble_link_work_t *work = NULL;
    esp_err_t result = ble_link_service_accept(
                           &facts, BLE_LINK_SERVICE_RX_SESSION, context->write_data,
                           context->write_len, &work);

    if (result != ESP_OK || work == NULL)
    {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (work_submit != NULL)
    {
        result = work_submit(work, work_submit_arg);
        if (result != ESP_OK)
        {
            ble_link_service_release_work(work);
            return 0x11;
        }
        return 0;
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
    if (s_gatt_mutex == NULL)
    {
        s_gatt_mutex = xSemaphoreCreateRecursiveMutexStatic(
                           &s_gatt_mutex_control);
        if (s_gatt_mutex == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    _ble_link_gatt_lock();
    s_gatt.config = *config;
    if (s_gatt.config.att_mtu < 23U)
    {
        s_gatt.config.att_mtu = 23U;
    }
    s_gatt.configured = true;
    const bool registered = s_gatt.registered;

    _ble_link_gatt_unlock();
    ble_link_session_init(config->boot_id);
    const size_t queue_depth = (config->tx_queue_depth > 0U) ?
                               config->tx_queue_depth : 32U;

    ble_link_service_init(config->boot_id, _ble_link_gatt_output, NULL,
                          NULL, queue_depth);
    if (!registered)
    {
        esp_err_t result = ble_gatt_registry_register(&s_service);

        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        {
            return result;
        }
        _ble_link_gatt_lock();
        s_gatt.registered = true;
        _ble_link_gatt_unlock();
    }
    return ESP_OK;
}

void ble_link_gatt_reset(void)
{
    _ble_link_gatt_lock();
    const ble_link_gatt_config_t config = s_gatt.config;
    const bool registered = s_gatt.registered;

    _ble_link_gatt_unlock();
    ble_link_service_reset();
    _ble_link_gatt_lock();
    memset(&s_gatt, 0, sizeof(s_gatt));
    s_gatt.config = config;
    s_gatt.configured = true;
    s_gatt.registered = registered;
    s_gatt.config.connection_generation = 0U;
    s_gatt.config.conn_handle = 0U;
    s_gatt.config.att_mtu = 23U;
    _ble_link_gatt_unlock();
}

esp_err_t ble_link_gatt_restart(void)
{
    _ble_link_gatt_lock();
    if (!s_gatt.configured)
    {
        _ble_link_gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint64_t boot_id = s_gatt.config.boot_id;
    const size_t queue_depth = (s_gatt.config.tx_queue_depth > 0U) ?
                               s_gatt.config.tx_queue_depth : 32U;

    s_gatt.config.connection_generation = 0U;
    s_gatt.config.conn_handle = 0U;
    s_gatt.config.att_mtu = 23U;
    _ble_link_gatt_unlock();
    ble_link_service_init(boot_id, _ble_link_gatt_output, NULL, NULL,
                          queue_depth);
    return ESP_OK;
}

void ble_link_gatt_update_handles(void)
{
    _ble_link_gatt_lock();
    for (size_t i = 0U; i < 2U; ++i)
    {
        uint16_t handle = 0U;

        if (ble_gatt_registry_get_assigned_handle(s_characteristics[i].uuid,
                &handle) == ESP_OK)
        {
            s_gatt.handles[i] = handle;
        }
    }
    _ble_link_gatt_unlock();
}

esp_err_t ble_link_gatt_refresh_link_state(void)
{
    return ESP_OK;
}

void ble_link_gatt_authentication_epoch_advance(void)
{
}

void ble_link_gatt_cccd_epoch_advance(void)
{
}

void ble_link_gatt_mark_link_state_dirty(void)
{
}

void ble_link_gatt_request_link_state_refresh(void)
{
}

bool ble_link_gatt_link_state_dirty(void)
{
    return false;
}

bool ble_link_gatt_link_state_retry_pending(void)
{
    return false;
}

uint32_t ble_link_gatt_link_state_retry_remaining_ms(void)
{
    return UINT32_MAX;
}

void ble_link_gatt_set_connection(
    uint32_t generation, uint16_t conn_handle,
    uint8_t peer_addr_type, const uint8_t peer_addr[6])
{
    _ble_link_gatt_lock();
    if (s_gatt.configured)
    {
        s_gatt.config.connection_generation = generation;
        s_gatt.config.conn_handle = conn_handle;
        s_gatt.config.peer_addr_type = peer_addr_type;
        if (peer_addr != NULL)
        {
            memcpy(s_gatt.config.peer_addr, peer_addr, 6U);
        }
    }
    _ble_link_gatt_unlock();
    ble_link_service_on_connect(generation, conn_handle);
}

esp_err_t ble_link_gatt_update_identity(
    uint32_t generation, uint16_t conn_handle,
    uint8_t peer_addr_type, const uint8_t peer_addr[6])
{
    _ble_link_gatt_lock();
    if (!s_gatt.configured ||
            s_gatt.config.connection_generation != generation ||
            s_gatt.config.conn_handle != conn_handle)
    {
        _ble_link_gatt_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    s_gatt.config.peer_addr_type = peer_addr_type;
    if (peer_addr != NULL)
    {
        memcpy(s_gatt.config.peer_addr, peer_addr, 6U);
    }
    _ble_link_gatt_unlock();
    return ESP_OK;
}

void ble_link_gatt_on_reassembly_idle_generation(
    uint32_t generation, uint32_t epoch)
{
    ble_link_service_idle_timeout_epoch(generation, epoch);
}

void ble_link_gatt_set_att_mtu(uint16_t mtu)
{
    _ble_link_gatt_lock();
    if (s_gatt.configured)
    {
        if (mtu > BLE_LINK_GATT_ATT_MTU_MAX)
        {
            mtu = BLE_LINK_GATT_ATT_MTU_MAX;
        }
        s_gatt.config.att_mtu = (mtu >= 23U) ? mtu : 23U;
    }
    _ble_link_gatt_unlock();
}

esp_err_t ble_link_gatt_get_att_mtu(uint32_t *out_mtu)
{
    if (out_mtu == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_gatt_lock();
    if (!s_gatt.configured)
    {
        _ble_link_gatt_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    *out_mtu = s_gatt.config.att_mtu;
    _ble_link_gatt_unlock();
    return ESP_OK;
}

uint16_t ble_link_gatt_link_state_handle(void)
{
    return 0U;
}

uint16_t ble_link_gatt_session_tx_handle(void)
{
    _ble_link_gatt_lock();
    const uint16_t handle = s_gatt.handles[1];

    _ble_link_gatt_unlock();
    return handle;
}

uint16_t ble_link_gatt_control_tx_handle(void)
{
    return 0U;
}

uint16_t ble_link_gatt_session_rx_handle(void)
{
    _ble_link_gatt_lock();
    const uint16_t handle = s_gatt.handles[0];

    _ble_link_gatt_unlock();
    return handle;
}

uint16_t ble_link_gatt_control_rx_handle(void)
{
    return 0U;
}
