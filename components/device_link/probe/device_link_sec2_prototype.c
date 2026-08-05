#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "protocomm.h"
#include "protocomm_security2.h"

#include "device_link_framing.h"
#include "device_link_sec2_prototype.h"

#define DBG_TAG "device_link_sec2"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define DEVICE_LINK_SEC2_PROTOTYPE_QUEUE_DEPTH 4U
#define DEVICE_LINK_SEC2_PROTOTYPE_SESSION_MAX 1024U
#define DEVICE_LINK_SEC2_PROTOTYPE_CONTROL_MAX 4096U
#define DEVICE_LINK_SEC2_PROTOTYPE_SESSION_ENDPOINT "sec2"
#define DEVICE_LINK_SEC2_PROTOTYPE_PING_ENDPOINT "link-ping"
#define DEVICE_LINK_SEC2_PROTOTYPE_PING_RESPONSE "link-v1-ok"
#define DEVICE_LINK_SEC2_PROTOTYPE_MAX_TX_CHUNK 487U
#define DEVICE_LINK_SEC2_PROTOTYPE_STOP_TIMEOUT_MS 2000U

#define DEVICE_LINK_SEC2_PROTOTYPE_SERVICE_UUID \
    0xa3, 0x4e, 0x85, 0x57, 0x11, 0x3d, 0x8a, 0xa2, \
    0x59, 0x4e, 0xbb, 0xb4, 0x92, 0x31, 0x20, 0x3e
#define DEVICE_LINK_SEC2_PROTOTYPE_SESSION_RX_UUID \
    0xa2, 0xf0, 0xcd, 0xfc, 0xe0, 0xe6, 0x5c, 0xb8, \
    0xd8, 0x4d, 0x4c, 0xcb, 0x43, 0xe6, 0x01, 0x48
#define DEVICE_LINK_SEC2_PROTOTYPE_SESSION_TX_UUID \
    0x05, 0x2a, 0xaf, 0xd2, 0x5f, 0xec, 0xa1, 0x83, \
    0x2c, 0x40, 0xac, 0xbe, 0x10, 0x57, 0xe8, 0x2b
#define DEVICE_LINK_SEC2_PROTOTYPE_CONTROL_RX_UUID \
    0xc8, 0x13, 0x3d, 0x40, 0x3d, 0xfb, 0x0c, 0x8e, \
    0x72, 0x47, 0x9d, 0x66, 0x62, 0x46, 0xa1, 0x81
#define DEVICE_LINK_SEC2_PROTOTYPE_CONTROL_TX_UUID \
    0x3a, 0x88, 0x03, 0x4c, 0xf6, 0xb8, 0x62, 0xb5, \
    0x9c, 0x4a, 0x40, 0x1e, 0xc7, 0x5a, 0x73, 0x11

typedef struct device_link_sec2_queue_item
{
    uint8_t *data;
    size_t length;
    uint32_t generation;
    bool session_channel;
} device_link_sec2_queue_item_t;

typedef struct device_link_sec2_receive_channel
{
    device_link_reassembler_t reassembler;
    QueueHandle_t queue;
    atomic_bool worker_busy;
} device_link_sec2_receive_channel_t;

typedef struct device_link_sec2_prototype
{
    const device_link_sec2_prototype_config_t *config;
    protocomm_t *protocomm;
    TaskHandle_t worker_task;
    SemaphoreHandle_t worker_exit;
    device_link_sec2_receive_channel_t session;
    device_link_sec2_receive_channel_t control;
    uint8_t session_buffer[DEVICE_LINK_SEC2_PROTOTYPE_SESSION_MAX];
    uint8_t control_buffer[DEVICE_LINK_SEC2_PROTOTYPE_CONTROL_MAX];
    uint16_t session_tx_val_handle;
    uint16_t control_tx_val_handle;
    atomic_uint connection_handle;
    atomic_uint connection_generation;
    atomic_uint negotiated_mtu;
    atomic_bool connected;
    atomic_bool tx_in_flight;
    atomic_int tx_result;
    atomic_uint tx_value_handle;
    atomic_bool running;
    bool session_open;
    uint32_t session_generation;
} device_link_sec2_prototype_t;

static device_link_sec2_prototype_t s_prototype;

static int _sec2_prototype_gap_event(
    struct ble_gap_event *event, void *arg);
static int _sec2_prototype_session_access(
    uint16_t connection_handle, uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context, void *arg);
static int _sec2_prototype_control_access(
    uint16_t connection_handle, uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context, void *arg);
static esp_err_t _sec2_prototype_ping_handler(
    uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
    uint8_t **outbuf, ssize_t *outlen, void *private_data);
static void _sec2_prototype_advertise(void);

static const ble_uuid128_t s_service_uuid =
{
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {DEVICE_LINK_SEC2_PROTOTYPE_SERVICE_UUID},
};
static const ble_uuid128_t s_session_rx_uuid =
{
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {DEVICE_LINK_SEC2_PROTOTYPE_SESSION_RX_UUID},
};
static const ble_uuid128_t s_session_tx_uuid =
{
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {DEVICE_LINK_SEC2_PROTOTYPE_SESSION_TX_UUID},
};
static const ble_uuid128_t s_control_rx_uuid =
{
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {DEVICE_LINK_SEC2_PROTOTYPE_CONTROL_RX_UUID},
};
static const ble_uuid128_t s_control_tx_uuid =
{
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {DEVICE_LINK_SEC2_PROTOTYPE_CONTROL_TX_UUID},
};

static const struct ble_gatt_svc_def s_session_svcs[] =
{
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                .uuid = &s_session_rx_uuid.u,
                .access_cb = _sec2_prototype_session_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_session_tx_uuid.u,
                .access_cb = _sec2_prototype_session_access,
                .flags = BLE_GATT_CHR_F_INDICATE,
                .val_handle = &s_prototype.session_tx_val_handle,
            },
            {
                .uuid = &s_control_rx_uuid.u,
                .access_cb = _sec2_prototype_control_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_control_tx_uuid.u,
                .access_cb = _sec2_prototype_control_access,
                .flags = BLE_GATT_CHR_F_INDICATE,
                .val_handle = &s_prototype.control_tx_val_handle,
            },
            {0},
        },
    },
    {0},
};

static esp_err_t _sec2_prototype_ping_handler(
    uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
    uint8_t **outbuf, ssize_t *outlen, void *private_data)
{
    (void)inbuf;
    (void)inlen;
    (void)private_data;
    if (outbuf == NULL || outlen == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *outbuf = (uint8_t *)strdup(DEVICE_LINK_SEC2_PROTOTYPE_PING_RESPONSE);
    if (*outbuf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    *outlen = (ssize_t)strlen(DEVICE_LINK_SEC2_PROTOTYPE_PING_RESPONSE);
    LOG_I("ping served session=%lu", (unsigned long)session_id);
    return ESP_OK;
}

static void _sec2_prototype_wake_worker(void)
{
    if (s_prototype.worker_task != NULL)
    {
        xTaskNotifyGive(s_prototype.worker_task);
    }
}

static void _sec2_prototype_session_close(void)
{
    if (s_prototype.session_open)
    {
        (void)protocomm_close_session(s_prototype.protocomm,
                                      s_prototype.session_generation);
        s_prototype.session_open = false;
        s_prototype.session_generation = 0U;
    }
}

static void _sec2_prototype_session_open_if_needed(uint32_t generation)
{
    if (!s_prototype.session_open)
    {
        const esp_err_t result = protocomm_open_session(
                                     s_prototype.protocomm, generation);
        if (result != ESP_OK)
        {
            LOG_E("session open failed generation=%lu result=%d",
                  (unsigned long)generation, result);
            return;
        }
        s_prototype.session_open = true;
        s_prototype.session_generation = generation;
    }
}

static int _sec2_prototype_gap_event(
    struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            const uint32_t generation =
                atomic_load(&s_prototype.connection_generation) + 1U;
            atomic_store(&s_prototype.connection_generation, generation);
            atomic_store(&s_prototype.connection_handle,
                         event->connect.conn_handle);
            atomic_store(&s_prototype.connected, true);
            atomic_store(&s_prototype.negotiated_mtu, 23U);
            device_link_reassembler_reset(&s_prototype.session.reassembler);
            device_link_reassembler_reset(&s_prototype.control.reassembler);
            LOG_I("connected generation=%lu",
                  (unsigned long)generation);
        }
        else
        {
            atomic_store(&s_prototype.connected, false);
            atomic_store(&s_prototype.connection_handle, 0U);
            _sec2_prototype_advertise();
            LOG_E("connect failed status=%d", event->connect.status);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        atomic_store(&s_prototype.tx_result, BLE_HS_ENOTCONN);
        atomic_store(&s_prototype.tx_in_flight, false);
        atomic_store(&s_prototype.connected, false);
        atomic_store(&s_prototype.connection_handle, 0U);
        device_link_reassembler_reset(&s_prototype.session.reassembler);
        device_link_reassembler_reset(&s_prototype.control.reassembler);
        _sec2_prototype_wake_worker();
        _sec2_prototype_advertise();
        LOG_I("disconnected reason=%d", event->disconnect.reason);
        return 0;
    case BLE_GAP_EVENT_MTU:
        atomic_store(&s_prototype.negotiated_mtu,
                     (unsigned int)event->mtu.value);
        LOG_I("mtu=%d", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (!event->notify_tx.indication ||
                event->notify_tx.conn_handle !=
                (uint16_t)atomic_load(&s_prototype.connection_handle) ||
                event->notify_tx.attr_handle !=
                (uint16_t)atomic_load(&s_prototype.tx_value_handle))
        {
            return 0;
        }
        if (event->notify_tx.status == 0)
        {
            return 0;
        }
        atomic_store(&s_prototype.tx_result, event->notify_tx.status);
        atomic_store(&s_prototype.tx_in_flight, false);
        _sec2_prototype_wake_worker();
        return 0;
    default:
        return 0;
    }
}

static esp_err_t _sec2_prototype_send_fragmented(
    uint16_t value_handle, const uint8_t *payload, size_t payload_len)
{
    const unsigned int negotiated_mtu =
        atomic_load(&s_prototype.negotiated_mtu);
    size_t chunk_limit = DEVICE_LINK_SEC2_PROTOTYPE_MAX_TX_CHUNK;
    const unsigned int value_limit = negotiated_mtu >= 4U ?
                                     negotiated_mtu - 3U : 0U;
    size_t offset = 0U;

    if (value_limit >= DEVICE_LINK_FRAMING_HEADER_BYTES + 1U)
    {
        const size_t mtu_chunk = value_limit -
                                 DEVICE_LINK_FRAMING_HEADER_BYTES;
        if (mtu_chunk < chunk_limit)
        {
            chunk_limit = mtu_chunk;
        }
    }
    if (chunk_limit == 0U)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    while (offset < payload_len)
    {
        const size_t remaining = payload_len - offset;
        const size_t chunk = remaining < chunk_limit ? remaining : chunk_limit;
        const bool first = offset == 0U;
        const bool last = remaining == chunk;
        device_link_fragment_header_t header =
        {
            .version = DEVICE_LINK_FRAMING_VERSION,
            .flags = (uint8_t)((first ? DEVICE_LINK_FRAMING_FLAG_START : 0U) |
                               (last ? DEVICE_LINK_FRAMING_FLAG_END : 0U)),
            .frame_id = 1U,
            .total_length = (uint16_t)payload_len,
            .offset = (uint16_t)offset,
        };
        uint8_t value[DEVICE_LINK_FRAMING_MAX_VALUE_BYTES];
        size_t value_len = 0U;
        struct os_mbuf *om;
        int result;

        if (device_link_framing_encode(&header, payload + offset, chunk,
                                       value, sizeof(value), &value_len) !=
                DEVICE_LINK_FRAME_OK)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        om = ble_hs_mbuf_from_flat(value, value_len);
        if (om == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        atomic_store(&s_prototype.tx_value_handle, value_handle);
        atomic_store(&s_prototype.tx_result, 0);
        atomic_store(&s_prototype.tx_in_flight, true);
        result = ble_gatts_indicate_custom(
                     (uint16_t)atomic_load(&s_prototype.connection_handle),
                     value_handle, om);
        if (result != 0)
        {
            atomic_store(&s_prototype.tx_in_flight, false);
            return ESP_FAIL;
        }
        while (atomic_load(&s_prototype.tx_in_flight))
        {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        if (atomic_load(&s_prototype.tx_result) != BLE_HS_EDONE)
        {
            LOG_E("indication failed status=%d",
                  atomic_load(&s_prototype.tx_result));
            return ESP_FAIL;
        }
        offset += chunk;
    }
    return ESP_OK;
}

static void _sec2_prototype_worker(void *arg)
{
    (void)arg;
    for (;;)
    {
        device_link_sec2_queue_item_t item;
        const char *endpoint;
        uint16_t value_handle;
        device_link_sec2_receive_channel_t *channel;
        uint8_t *response = NULL;
        ssize_t response_len = 0;
        esp_err_t result;
        bool received = false;

        if (!atomic_load(&s_prototype.running))
        {
            if (s_prototype.worker_exit != NULL)
            {
                xSemaphoreGive(s_prototype.worker_exit);
            }
            break;
        }
        if (xQueueReceive(s_prototype.control.queue, &item, 0U) == pdTRUE)
        {
            endpoint = DEVICE_LINK_SEC2_PROTOTYPE_PING_ENDPOINT;
            value_handle = s_prototype.control_tx_val_handle;
            channel = &s_prototype.control;
            received = true;
        }
        else if (xQueueReceive(s_prototype.session.queue, &item, 0U) == pdTRUE)
        {
            endpoint = DEVICE_LINK_SEC2_PROTOTYPE_SESSION_ENDPOINT;
            value_handle = s_prototype.session_tx_val_handle;
            channel = &s_prototype.session;
            received = true;
        }
        else
        {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!received)
        {
            continue;
        }
        if (!atomic_load(&s_prototype.connected) ||
                item.generation !=
                atomic_load(&s_prototype.connection_generation))
        {
            atomic_store(&channel->worker_busy, false);
            _sec2_prototype_session_close();
            free(item.data);
            continue;
        }
        _sec2_prototype_session_open_if_needed(item.generation);
        if (!s_prototype.session_open)
        {
            atomic_store(&channel->worker_busy, false);
            free(item.data);
            continue;
        }
        result = protocomm_req_handle(
                     s_prototype.protocomm, endpoint, item.generation,
                     item.data, (ssize_t)item.length,
                     &response, &response_len);
        free(item.data);
        if (result == ESP_OK && response != NULL && response_len > 0)
        {
            result = _sec2_prototype_send_fragmented(
                         value_handle, response, (size_t)response_len);
        }
        else if (result != ESP_OK)
        {
            LOG_E("request failed endpoint=%s result=%d", endpoint, result);
        }
        free(response);
        if (result != ESP_OK || !atomic_load(&s_prototype.connected))
        {
            _sec2_prototype_session_close();
        }
        atomic_store(&channel->worker_busy, false);
    }
}

static int _sec2_prototype_channel_write(
    device_link_sec2_receive_channel_t *channel, bool session_channel,
    struct ble_gatt_access_ctxt *context)
{
    uint8_t *value;
    uint16_t value_len = 0U;
    const uint16_t om_len = OS_MBUF_PKTLEN(context->om);

    if (atomic_load(&channel->worker_busy))
    {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (om_len == 0U)
    {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    value = malloc(om_len);
    if (value == NULL)
    {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ble_hs_mbuf_to_flat(context->om, value, om_len, &value_len) != 0)
    {
        free(value);
        return BLE_ATT_ERR_UNLIKELY;
    }
    size_t delivered_len = 0U;
    const device_link_frame_result_t result =
        device_link_reassembler_feed(&channel->reassembler, value,
                                     value_len, &delivered_len);
    free(value);
    if (result == DEVICE_LINK_FRAME_REJECTED)
    {
        device_link_reassembler_reset(&channel->reassembler);
        return BLE_ATT_ERR_INVALID_PDU;
    }
    if (result == DEVICE_LINK_FRAME_COMPLETE)
    {
        device_link_sec2_queue_item_t item;

        item.data = malloc(delivered_len);
        if (item.data == NULL)
        {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        item.length = delivered_len;
        item.generation = atomic_load(&s_prototype.connection_generation);
        item.session_channel = session_channel;
        memcpy(item.data, channel->reassembler.buffer, delivered_len);
        device_link_reassembler_reset(&channel->reassembler);
        atomic_store(&channel->worker_busy, true);
        if (xQueueSend(channel->queue, &item, 0U) != pdTRUE)
        {
            atomic_store(&channel->worker_busy, false);
            free(item.data);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        _sec2_prototype_wake_worker();
    }
    return 0;
}

static int _sec2_prototype_session_access(
    uint16_t connection_handle, uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context, void *arg)
{
    (void)attribute_handle;
    (void)arg;
    if (connection_handle !=
            (uint16_t)atomic_load(&s_prototype.connection_handle) ||
            !atomic_load(&s_prototype.connected))
    {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        return _sec2_prototype_channel_write(&s_prototype.session, true,
                                             context);
    }
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int _sec2_prototype_control_access(
    uint16_t connection_handle, uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context, void *arg)
{
    (void)attribute_handle;
    (void)arg;
    if (connection_handle !=
            (uint16_t)atomic_load(&s_prototype.connection_handle) ||
            !atomic_load(&s_prototype.connected))
    {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        return _sec2_prototype_channel_write(&s_prototype.control, false,
                                             context);
    }
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static void _sec2_prototype_reset(int reason)
{
    LOG_E("nimble reset reason=%d", reason);
}

static void _sec2_prototype_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    int result;

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                               &adv_params, &_sec2_prototype_gap_event, NULL);
    LOG_I("advertising start result=%d", result);
}

static void _sec2_prototype_sync(void)
{
    struct ble_hs_adv_fields fields;
    int result;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_prototype.config->device_name;
    fields.name_len = (uint8_t)strlen(s_prototype.config->device_name);
    result = ble_gap_adv_set_fields(&fields);
    if (result != 0)
    {
        LOG_E("adv fields failed result=%d", result);
        return;
    }
    _sec2_prototype_advertise();
}

static void _sec2_prototype_gatts_register(
    struct ble_gatt_register_ctxt *context, void *arg)
{
    (void)arg;
    if (context->op == BLE_GATT_REGISTER_OP_SVC)
    {
        LOG_I("session service registered");
    }
}

static void _sec2_prototype_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t _sec2_prototype_nimble_init(void)
{
    int result;

    result = nimble_port_init();
    if (result != ESP_OK)
    {
        return ESP_FAIL;
    }
    ble_hs_cfg.reset_cb = _sec2_prototype_reset;
    ble_hs_cfg.sync_cb = _sec2_prototype_sync;
    ble_hs_cfg.gatts_register_cb = _sec2_prototype_gatts_register;
    ble_hs_cfg.store_status_cb = NULL;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    result = ble_gatts_count_cfg(s_session_svcs);
    if (result != 0)
    {
        nimble_port_deinit();
        return ESP_FAIL;
    }
    result = ble_gatts_add_svcs(s_session_svcs);
    if (result != 0)
    {
        nimble_port_deinit();
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void _sec2_prototype_drain_queue(QueueHandle_t queue)
{
    device_link_sec2_queue_item_t item;

    while (xQueueReceive(queue, &item, 0U) == pdTRUE)
    {
        free(item.data);
    }
}

static void _sec2_prototype_cleanup(void)
{
    if (s_prototype.session.queue != NULL)
    {
        _sec2_prototype_drain_queue(s_prototype.session.queue);
        vQueueDelete(s_prototype.session.queue);
        s_prototype.session.queue = NULL;
    }
    if (s_prototype.control.queue != NULL)
    {
        _sec2_prototype_drain_queue(s_prototype.control.queue);
        vQueueDelete(s_prototype.control.queue);
        s_prototype.control.queue = NULL;
    }
    if (s_prototype.protocomm != NULL)
    {
        protocomm_delete(s_prototype.protocomm);
        s_prototype.protocomm = NULL;
    }
}

esp_err_t device_link_sec2_prototype_start(
    const device_link_sec2_prototype_config_t *config)
{
    esp_err_t result;

    if (config == NULL || config->salt == NULL || config->verifier == NULL ||
            config->device_name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&s_prototype.running))
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_prototype, 0, sizeof(s_prototype));
    s_prototype.config = config;
    atomic_store(&s_prototype.negotiated_mtu, 23U);
    atomic_store(&s_prototype.running, true);
    s_prototype.worker_exit = xSemaphoreCreateBinary();
    if (s_prototype.worker_exit == NULL)
    {
        atomic_store(&s_prototype.running, false);
        return ESP_ERR_NO_MEM;
    }
    s_prototype.session.queue = xQueueCreate(
                                    DEVICE_LINK_SEC2_PROTOTYPE_QUEUE_DEPTH,
                                    sizeof(device_link_sec2_queue_item_t));
    s_prototype.control.queue = xQueueCreate(
                                    DEVICE_LINK_SEC2_PROTOTYPE_QUEUE_DEPTH,
                                    sizeof(device_link_sec2_queue_item_t));
    if (s_prototype.session.queue == NULL ||
            s_prototype.control.queue == NULL)
    {
        _sec2_prototype_cleanup();
        atomic_store(&s_prototype.running, false);
        return ESP_ERR_NO_MEM;
    }
    device_link_reassembler_init(&s_prototype.session.reassembler,
                                 s_prototype.session_buffer,
                                 sizeof(s_prototype.session_buffer));
    device_link_reassembler_init(&s_prototype.control.reassembler,
                                 s_prototype.control_buffer,
                                 sizeof(s_prototype.control_buffer));

    s_prototype.protocomm = protocomm_new();
    if (s_prototype.protocomm == NULL)
    {
        _sec2_prototype_cleanup();
        atomic_store(&s_prototype.running, false);
        return ESP_ERR_NO_MEM;
    }
    const protocomm_security2_params_t security =
    {
        .salt = config->salt,
        .salt_len = config->salt_len,
        .verifier = config->verifier,
        .verifier_len = (uint16_t)config->verifier_len,
    };
    result = protocomm_set_security(
                 s_prototype.protocomm,
                 DEVICE_LINK_SEC2_PROTOTYPE_SESSION_ENDPOINT,
                 &protocomm_security2, &security);
    if (result != ESP_OK)
    {
        _sec2_prototype_cleanup();
        atomic_store(&s_prototype.running, false);
        return result;
    }
    result = protocomm_add_endpoint(
                 s_prototype.protocomm,
                 DEVICE_LINK_SEC2_PROTOTYPE_PING_ENDPOINT,
                 _sec2_prototype_ping_handler, NULL);
    if (result != ESP_OK)
    {
        _sec2_prototype_cleanup();
        atomic_store(&s_prototype.running, false);
        return result;
    }

    result = _sec2_prototype_nimble_init();
    if (result != ESP_OK)
    {
        _sec2_prototype_cleanup();
        atomic_store(&s_prototype.running, false);
        return result;
    }
    if (xTaskCreate(_sec2_prototype_worker, "sec2_proto",
                    config->task_stack_bytes, NULL, config->task_priority,
                    &s_prototype.worker_task) != pdPASS)
    {
        nimble_port_deinit();
        _sec2_prototype_cleanup();
        atomic_store(&s_prototype.running, false);
        return ESP_ERR_NO_MEM;
    }
    nimble_port_freertos_init(_sec2_prototype_host_task);
    return ESP_OK;
}

void device_link_sec2_prototype_stop(void)
{
    if (!atomic_load(&s_prototype.running))
    {
        return;
    }
    atomic_store(&s_prototype.running, false);
    if (s_prototype.worker_task != NULL)
    {
        _sec2_prototype_wake_worker();
        if (s_prototype.worker_exit != NULL)
        {
            (void)xSemaphoreTake(s_prototype.worker_exit,
                                 pdMS_TO_TICKS(
                                     DEVICE_LINK_SEC2_PROTOTYPE_STOP_TIMEOUT_MS));
        }
        vTaskDelete(s_prototype.worker_task);
        s_prototype.worker_task = NULL;
    }
    if (s_prototype.worker_exit != NULL)
    {
        vSemaphoreDelete(s_prototype.worker_exit);
        s_prototype.worker_exit = NULL;
    }
    if (nimble_port_stop() == 0)
    {
        nimble_port_deinit();
    }
    _sec2_prototype_cleanup();
}
