#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_gap_manager.h"
#include "ble_gatt_registry.h"
#include "ble_nimble_port.h"
#include "ble_runtime.h"

#define DBG_TAG "ble_nimble_port"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS 10000U
#define BLE_NIMBLE_PORT_ACCESS_BUFFER_BYTES 512U

#if CONFIG_BT_NIMBLE_PINNED_TO_CORE == 0
    #define BLE_NIMBLE_PORT_HOST_CORE 0
#elif CONFIG_BT_NIMBLE_PINNED_TO_CORE == 1
    #define BLE_NIMBLE_PORT_HOST_CORE 1
#else
    #define BLE_NIMBLE_PORT_HOST_CORE tskNO_AFFINITY
#endif

typedef struct ble_nimble_port
{
    SemaphoreHandle_t sync_semaphore;
    SemaphoreHandle_t exit_semaphore;
    TaskHandle_t host_task;
    ble_nimble_port_gap_cb_t gap_callback;
    void *gap_arg;
    struct ble_gap_event_listener listener;
    bool listener_registered;
    bool started;
    bool deinitialized;
    bool deinit_failed;
    esp_err_t deinit_error;
} ble_nimble_port_t;

static ble_nimble_port_t s_port;

static int _ble_nimble_port_gap_event(
    struct ble_gap_event *event, void *arg)
{
    (void)arg;
    ble_gap_manager_event_t manager_event;

    memset(&manager_event, 0, sizeof(manager_event));
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        manager_event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
        manager_event.conn_handle = event->connect.conn_handle;
        manager_event.status = event->connect.status;
        if (event->connect.status == 0 &&
                ble_gap_manager_handle_event(&manager_event) ==
                ESP_ERR_NO_MEM)
        {
            const int terminate_result = ble_gap_terminate(
                                             event->connect.conn_handle,
                                             BLE_ERR_CONN_TERM_LOCAL);

            if (terminate_result != 0)
            {
                LOG_W("rejected connection terminate failed result=%d",
                      terminate_result);
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        manager_event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
        manager_event.conn_handle = event->disconnect.conn.conn_handle;
        manager_event.reason = event->disconnect.reason;
        (void)ble_gap_manager_handle_event(&manager_event);
        break;
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.channel_id != BLE_L2CAP_CID_ATT)
        {
            break;
        }
        manager_event.type = BLE_GAP_MANAGER_EVENT_MTU;
        manager_event.conn_handle = event->mtu.conn_handle;
        manager_event.mtu = event->mtu.value;
        (void)ble_gap_manager_handle_event(&manager_event);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE;
        manager_event.conn_handle = event->enc_change.conn_handle;
        {
            struct ble_gap_conn_desc description;

            manager_event.encrypted =
                ble_gap_conn_find(event->enc_change.conn_handle,
                                  &description) == 0 &&
                description.sec_state.encrypted;
        }
        (void)ble_gap_manager_handle_event(&manager_event);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE;
        manager_event.conn_handle = event->subscribe.conn_handle;
        manager_event.attr_handle = event->subscribe.attr_handle;
        manager_event.subscribed = event->subscribe.cur_notify ||
                                   event->subscribe.cur_indicate;
        (void)ble_gap_manager_handle_event(&manager_event);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_ADV_COMPLETE;
        (void)ble_gap_manager_handle_event(&manager_event);
        break;
    default:
        break;
    }
    if (s_port.gap_callback != NULL)
    {
        s_port.gap_callback(event, s_port.gap_arg);
    }
    return 0;
}

static void _ble_nimble_port_on_sync(void)
{
    if (ble_hs_synced())
    {
        LOG_I("host synchronized");
        xSemaphoreGive(s_port.sync_semaphore);
    }
}

static void _ble_nimble_port_on_reset(int reason)
{
    LOG_E("host reset reason=%d", reason);
}

static int _ble_nimble_port_access_bridge(
    uint16_t conn_handle, uint16_t attr_handle,
    struct ble_gatt_access_ctxt *context, void *arg)
{
    (void)arg;
    const ble_gatt_registry_characteristic_t *characteristic = NULL;
    uint8_t access_buffer[BLE_NIMBLE_PORT_ACCESS_BUFFER_BYTES];
    ble_gatt_registry_access_context_t port_context;
    uint16_t read_len = 0U;

    if (ble_gatt_registry_lookup_by_handle(attr_handle, &characteristic) !=
            ESP_OK)
    {
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }
    memset(&port_context, 0, sizeof(port_context));
    port_context.read_out = access_buffer;
    port_context.read_capacity = sizeof(access_buffer);
    port_context.read_len = &read_len;
    switch (context->op)
    {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        port_context.op = BLE_GATT_REGISTRY_OP_READ_CHR;
        port_context.offset = context->offset;
        break;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        port_context.op = BLE_GATT_REGISTRY_OP_WRITE_CHR;
        if (context->om != NULL)
        {
            uint16_t write_len = 0U;

            if (ble_hs_mbuf_to_flat(context->om, access_buffer,
                                    sizeof(access_buffer),
                                    &write_len) != 0)
            {
                return BLE_ATT_ERR_UNLIKELY;
            }
            port_context.write_data = access_buffer;
            port_context.write_len = write_len;
        }
        break;
    case BLE_GATT_ACCESS_OP_READ_DSC:
        port_context.op = BLE_GATT_REGISTRY_OP_READ_DSC;
        break;
    case BLE_GATT_ACCESS_OP_WRITE_DSC:
        port_context.op = BLE_GATT_REGISTRY_OP_WRITE_DSC;
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
    const int result = characteristic->access_cb(
                           conn_handle, attr_handle, &port_context,
                           characteristic->arg);
    if (result == 0 &&
            port_context.op == BLE_GATT_REGISTRY_OP_READ_CHR &&
            read_len > 0U)
    {
        if (read_len > port_context.read_capacity)
        {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (os_mbuf_append(context->om, access_buffer, read_len) != 0)
        {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }
    return result;
}

static void _ble_nimble_port_gatts_register(
    struct ble_gatt_register_ctxt *context, void *arg)
{
    (void)arg;
    if (context->op == BLE_GATT_REGISTER_OP_CHR &&
            context->chr.chr_def->uuid->type == BLE_UUID_TYPE_128)
    {
        const uint8_t *uuid_flat =
            ((const ble_uuid128_t *)context->chr.chr_def->uuid)->value;
        const esp_err_t result = ble_gatt_registry_assign_handle(
                                     uuid_flat, context->chr.val_handle);

        if (result != ESP_OK)
        {
            LOG_W("handle assignment failed result=%d", result);
        }
    }
}

static esp_err_t _ble_nimble_port_register_database(void)
{
    static struct ble_gatt_svc_def services[
            BLE_GATT_REGISTRY_MAX_SERVICES + 1];
    static struct ble_gatt_chr_def characteristic_sets[
            BLE_GATT_REGISTRY_MAX_SERVICES]
        [BLE_GATT_REGISTRY_MAX_CHARACTERISTICS + 1];
    static ble_uuid128_t service_uuids[BLE_GATT_REGISTRY_MAX_SERVICES];
    static ble_uuid128_t characteristic_uuids[
        BLE_GATT_REGISTRY_MAX_SERVICES]
    [BLE_GATT_REGISTRY_MAX_CHARACTERISTICS];
    size_t service_count = 0U;
    size_t characteristic_cursor = 0U;

    ble_gatt_registry_clear_handles();
    for (;;)
    {
        const ble_gatt_registry_service_t *service = NULL;

        if (ble_gatt_registry_get_service(service_count, &service) != ESP_OK)
        {
            break;
        }
        services[service_count].type = BLE_GATT_SVC_TYPE_PRIMARY;
        memcpy(service_uuids[service_count].value, service->uuid, 16U);
        service_uuids[service_count].u.type = BLE_UUID_TYPE_128;
        services[service_count].uuid = &service_uuids[service_count].u;

        for (size_t i = 0U; i < service->characteristic_count; ++i)
        {
            const ble_gatt_registry_characteristic_t *characteristic = NULL;

            if (ble_gatt_registry_get_characteristic(
                        characteristic_cursor + i, &characteristic) != ESP_OK)
            {
                return ESP_FAIL;
            }
            struct ble_gatt_chr_def *definition =
                    &characteristic_sets[service_count][i];

            memcpy(characteristic_uuids[service_count][i].value,
                   characteristic->uuid, 16U);
            characteristic_uuids[service_count][i].u.type = BLE_UUID_TYPE_128;
            definition->uuid = &characteristic_uuids[service_count][i].u;
            definition->access_cb = _ble_nimble_port_access_bridge;
            definition->arg = NULL;
            definition->flags = 0;
            if (characteristic->properties & BLE_GATT_REGISTRY_PROP_READ)
            {
                definition->flags |= BLE_GATT_CHR_F_READ;
            }
            if (characteristic->properties & BLE_GATT_REGISTRY_PROP_WRITE)
            {
                definition->flags |= BLE_GATT_CHR_F_WRITE;
            }
            if (characteristic->properties &
                    BLE_GATT_REGISTRY_PROP_WRITE_NO_RESPONSE)
            {
                definition->flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
            }
            if (characteristic->properties & BLE_GATT_REGISTRY_PROP_NOTIFY)
            {
                definition->flags |= BLE_GATT_CHR_F_NOTIFY;
            }
            if (characteristic->properties & BLE_GATT_REGISTRY_PROP_INDICATE)
            {
                definition->flags |= BLE_GATT_CHR_F_INDICATE;
            }
        }
        characteristic_sets[service_count]
        [service->characteristic_count].uuid = NULL;
        services[service_count].characteristics =
            characteristic_sets[service_count];
        characteristic_cursor += service->characteristic_count;
        service_count++;
    }
    services[service_count].type = 0;
    services[service_count].uuid = NULL;
    services[service_count].characteristics = NULL;

    int result = ble_gatts_count_cfg(services);
    if (result != 0)
    {
        return ESP_FAIL;
    }
    result = ble_gatts_add_svcs(services);
    if (result != 0)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_init(void)
{
    esp_err_t result;

    if (s_port.deinit_failed)
    {
        return s_port.deinit_error;
    }
    ble_gap_manager_init();
    result = nimble_port_init();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_port.listener_registered)
    {
        const int register_result = ble_gap_event_listener_register(
                                        &s_port.listener,
                                        _ble_nimble_port_gap_event, NULL);

        if (register_result != 0 && register_result != BLE_HS_EALREADY)
        {
            nimble_port_deinit();
            return ESP_FAIL;
        }
        s_port.listener_registered = true;
    }
    s_port.deinitialized = false;
    ble_hs_cfg.reset_cb = _ble_nimble_port_on_reset;
    ble_hs_cfg.sync_cb = _ble_nimble_port_on_sync;
    ble_hs_cfg.gatts_register_cb = _ble_nimble_port_gatts_register;
    ble_hs_cfg.store_status_cb = NULL;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    result = _ble_nimble_port_register_database();
    if (result != ESP_OK)
    {
        if (s_port.listener_registered)
        {
            (void)ble_gap_event_listener_unregister(&s_port.listener);
            s_port.listener_registered = false;
        }
        const esp_err_t cleanup = nimble_port_deinit();

        s_port.deinitialized = true;
        if (cleanup != ESP_OK)
        {
            s_port.deinit_failed = true;
            s_port.deinit_error = cleanup;
            LOG_E("database registration cleanup failed result=%d", cleanup);
            return cleanup;
        }
        return result;
    }
    return ESP_OK;
}

static void _ble_nimble_port_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    xSemaphoreGive(s_port.exit_semaphore);
    vTaskDelete(NULL);
}

static void _ble_nimble_port_drain_sync(void)
{
    while (xSemaphoreTake(s_port.sync_semaphore, 0U) == pdTRUE)
    {
        /* Discard stale sync tokens from a previous run. */
    }
}

static esp_err_t _ble_nimble_port_start(void)
{
    if (s_port.sync_semaphore == NULL)
    {
        s_port.sync_semaphore = xSemaphoreCreateBinary();
    }
    if (s_port.exit_semaphore == NULL)
    {
        s_port.exit_semaphore = xSemaphoreCreateBinary();
    }
    if (s_port.sync_semaphore == NULL || s_port.exit_semaphore == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    _ble_nimble_port_drain_sync();
    if (xTaskCreatePinnedToCore(_ble_nimble_port_host_task, "nimble_host",
                                CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, NULL,
                                (configMAX_PRIORITIES - 4),
                                &s_port.host_task,
                                BLE_NIMBLE_PORT_HOST_CORE) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    s_port.started = true;
    if (xSemaphoreTake(s_port.sync_semaphore,
                       pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) !=
            pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_stop(void)
{
    if (s_port.started)
    {
        const int result = nimble_port_stop();

        if (result != 0 && result != BLE_HS_EALREADY)
        {
            return ESP_FAIL;
        }
        if (xSemaphoreTake(s_port.exit_semaphore,
                           pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) !=
                pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }
        s_port.started = false;
    }
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_deinit(void)
{
    if (s_port.deinit_failed)
    {
        return s_port.deinit_error;
    }
    if (s_port.listener_registered)
    {
        (void)ble_gap_event_listener_unregister(&s_port.listener);
        s_port.listener_registered = false;
    }
    if (!s_port.deinitialized)
    {
        const esp_err_t result = nimble_port_deinit();

        if (result != ESP_OK)
        {
            s_port.deinit_failed = true;
            s_port.deinit_error = result;
            return result;
        }
        s_port.deinitialized = true;
    }
    if (s_port.sync_semaphore != NULL)
    {
        vSemaphoreDelete(s_port.sync_semaphore);
        s_port.sync_semaphore = NULL;
    }
    if (s_port.exit_semaphore != NULL)
    {
        vSemaphoreDelete(s_port.exit_semaphore);
        s_port.exit_semaphore = NULL;
    }
    return ESP_OK;
}

static const ble_runtime_host_port_t s_nimble_port =
{
    .init = _ble_nimble_port_init,
    .start = _ble_nimble_port_start,
    .stop = _ble_nimble_port_stop,
    .deinit = _ble_nimble_port_deinit,
};

esp_err_t ble_nimble_port_set_gap_callback(
    ble_nimble_port_gap_cb_t callback, void *arg)
{
    if (s_port.started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_port.gap_callback = callback;
    s_port.gap_arg = arg;
    return ESP_OK;
}

const ble_runtime_host_port_t *ble_nimble_port_get(void)
{
    return &s_nimble_port;
}
