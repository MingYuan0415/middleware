#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
#include "ble_port_ops.h"
#include "ble_runtime.h"

#define DBG_TAG "ble_nimble_port"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS 10000U
#define BLE_NIMBLE_PORT_ADV_QUIT_TIMEOUT_MS 2000U
#define BLE_NIMBLE_PORT_ACCESS_BUFFER_BYTES 512U
#define BLE_NIMBLE_PORT_ADV_QUEUE_DEPTH 4U
#define BLE_NIMBLE_PORT_ADV_TASK_STACK 2048U
#define BLE_NIMBLE_PORT_ADV_TASK_PRIORITY 2U

#if CONFIG_BT_NIMBLE_PINNED_TO_CORE == 0
    #define BLE_NIMBLE_PORT_HOST_CORE 0
#elif CONFIG_BT_NIMBLE_PINNED_TO_CORE == 1
    #define BLE_NIMBLE_PORT_HOST_CORE 1
#else
    #define BLE_NIMBLE_PORT_HOST_CORE tskNO_AFFINITY
#endif

typedef enum
{
    BLE_NIMBLE_PORT_ADV_CMD_START = 0,
    BLE_NIMBLE_PORT_ADV_CMD_STOP,
    BLE_NIMBLE_PORT_ADV_CMD_QUIT,
} ble_nimble_port_adv_cmd_type_t;

typedef struct ble_nimble_port_adv_cmd
{
    ble_nimble_port_adv_cmd_type_t type;
    const ble_port_adv_config_t *config;
} ble_nimble_port_adv_cmd_t;

typedef struct ble_nimble_port
{
    SemaphoreHandle_t sync_semaphore;
    SemaphoreHandle_t exit_semaphore;
    SemaphoreHandle_t stop_done_semaphore;
    SemaphoreHandle_t adv_exit_semaphore;
    TaskHandle_t host_task;
    TaskHandle_t adv_task;
    QueueHandle_t adv_queue;
    struct ble_gap_event_listener listener;
    bool listener_registered;
    const ble_port_ops_t *ops;
    bool started;
    bool deinitialized;
    bool nimble_init_attempted;
    bool quiescing;
    bool deinit_failed;
    esp_err_t deinit_error;
    int stop_result;
} ble_nimble_port_t;

static ble_nimble_port_t s_port;

static esp_err_t _ble_nimble_port_production_adv_start(
    const ble_port_adv_config_t *config);
static esp_err_t _ble_nimble_port_production_adv_stop(void);
static esp_err_t _ble_nimble_port_production_notify(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len);
static esp_err_t _ble_nimble_port_production_indicate(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len);

static const ble_port_ops_t s_production_ops =
{
    .adv_start = _ble_nimble_port_production_adv_start,
    .adv_stop = _ble_nimble_port_production_adv_stop,
    .notify = _ble_nimble_port_production_notify,
    .indicate = _ble_nimble_port_production_indicate,
};

static void _ble_nimble_port_dispatch(const ble_port_event_t *event)
{
    (void)ble_event_router_dispatch(event);
}

static void _ble_nimble_port_gap_consumer(
    const ble_port_event_t *event, void *arg)
{
    (void)arg;
    ble_gap_manager_event_t manager_event;

    memset(&manager_event, 0, sizeof(manager_event));
    switch (event->type)
    {
    case BLE_PORT_EVENT_CONNECT:
        manager_event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
        manager_event.conn_handle = event->conn_handle;
        manager_event.status = event->status;
        break;
    case BLE_PORT_EVENT_DISCONNECT:
        manager_event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
        manager_event.conn_handle = event->conn_handle;
        manager_event.reason = event->reason;
        break;
    case BLE_PORT_EVENT_MTU:
        manager_event.type = BLE_GAP_MANAGER_EVENT_MTU;
        manager_event.conn_handle = event->conn_handle;
        manager_event.mtu = event->mtu;
        break;
    case BLE_PORT_EVENT_ENC_CHANGE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE;
        manager_event.conn_handle = event->conn_handle;
        manager_event.encrypted = event->encrypted;
        break;
    case BLE_PORT_EVENT_SUBSCRIBE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE;
        manager_event.conn_handle = event->conn_handle;
        manager_event.attr_handle = event->attr_handle;
        manager_event.subscribed = event->subscribed;
        break;
    case BLE_PORT_EVENT_ADV_COMPLETE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_ADV_COMPLETE;
        break;
    default:
        return;
    }
    (void)ble_gap_manager_handle_event(&manager_event);
}

static int _ble_nimble_port_gap_event(
    struct ble_gap_event *event, void *arg)
{
    (void)arg;
    ble_port_event_t port_event;

    memset(&port_event, 0, sizeof(port_event));
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        port_event.type = BLE_PORT_EVENT_CONNECT;
        port_event.conn_handle = event->connect.conn_handle;
        port_event.status = event->connect.status;
        if (event->connect.status == 0)
        {
            _ble_nimble_port_dispatch(&port_event);
            ble_gap_manager_snapshot_t snapshot;

            if (ble_gap_manager_get_snapshot(&snapshot) == ESP_OK &&
                    (!snapshot.connected ||
                     snapshot.conn_handle != event->connect.conn_handle))
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
            return 0;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        port_event.type = BLE_PORT_EVENT_DISCONNECT;
        port_event.conn_handle = event->disconnect.conn.conn_handle;
        port_event.reason = event->disconnect.reason;
        break;
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.channel_id != BLE_L2CAP_CID_ATT)
        {
            return 0;
        }
        port_event.type = BLE_PORT_EVENT_MTU;
        port_event.conn_handle = event->mtu.conn_handle;
        port_event.mtu = event->mtu.value;
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        port_event.type = BLE_PORT_EVENT_ENC_CHANGE;
        port_event.conn_handle = event->enc_change.conn_handle;
        {
            struct ble_gap_conn_desc description;

            port_event.encrypted =
                ble_gap_conn_find(event->enc_change.conn_handle,
                                  &description) == 0 &&
                description.sec_state.encrypted;
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        port_event.type = BLE_PORT_EVENT_SUBSCRIBE;
        port_event.conn_handle = event->subscribe.conn_handle;
        port_event.attr_handle = event->subscribe.attr_handle;
        port_event.subscribed = event->subscribe.cur_notify ||
                                event->subscribe.cur_indicate;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        port_event.type = BLE_PORT_EVENT_ADV_COMPLETE;
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        port_event.type = BLE_PORT_EVENT_NOTIFY_TX;
        port_event.conn_handle = event->notify_tx.conn_handle;
        port_event.attr_handle = event->notify_tx.attr_handle;
        port_event.status = event->notify_tx.status;
        port_event.indication = event->notify_tx.indication != 0;
        if (event->notify_tx.status == 0)
        {
            port_event.tx_result = BLE_PORT_TX_SENT;
        }
        else if (event->notify_tx.status == BLE_HS_EDONE)
        {
            port_event.tx_result = BLE_PORT_TX_CONFIRMED;
        }
        else if (event->notify_tx.status == BLE_HS_ETIMEOUT)
        {
            port_event.tx_result = BLE_PORT_TX_TIMEOUT;
        }
        else
        {
            port_event.tx_result = BLE_PORT_TX_ERROR;
        }
        break;
    default:
        return 0;
    }
    _ble_nimble_port_dispatch(&port_event);
    return 0;
}

static void _ble_nimble_port_on_sync(void)
{
    if (ble_hs_synced())
    {
        ble_port_event_t event;

        LOG_I("host synchronized");
        xSemaphoreGive(s_port.sync_semaphore);
        memset(&event, 0, sizeof(event));
        event.type = BLE_PORT_EVENT_SYNC;
        _ble_nimble_port_dispatch(&event);
    }
}

static void _ble_nimble_port_on_reset(int reason)
{
    ble_port_event_t event;

    LOG_E("host reset reason=%d", reason);
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_RESET;
    event.status = reason;
    _ble_nimble_port_dispatch(&event);
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

static esp_err_t _ble_nimble_port_production_adv_start(
    const ble_port_adv_config_t *config)
{
    ble_nimble_port_adv_cmd_t cmd;

    if (config == NULL || s_port.adv_queue == NULL || s_port.quiescing)
    {
        return ESP_ERR_INVALID_STATE;
    }
    cmd.type = BLE_NIMBLE_PORT_ADV_CMD_START;
    cmd.config = config;
    if (xQueueSend(s_port.adv_queue, &cmd, 0U) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_production_adv_stop(void)
{
    ble_nimble_port_adv_cmd_t cmd;

    if (s_port.adv_queue == NULL || s_port.quiescing)
    {
        return ESP_ERR_INVALID_STATE;
    }
    cmd.type = BLE_NIMBLE_PORT_ADV_CMD_STOP;
    cmd.config = NULL;
    if (xQueueSend(s_port.adv_queue, &cmd, 0U) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void _ble_nimble_port_adv_start_execute(
    const ble_port_adv_config_t *config)
{
    struct ble_hs_adv_fields fields;
    int result;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    if (config->short_name != NULL && config->short_name_len > 0U)
    {
        fields.name = (uint8_t *)config->short_name;
        fields.name_len = (uint8_t)config->short_name_len;
        fields.name_is_complete = 0;
    }
    if (config->service_uuid != NULL)
    {
        static uint8_t service_data[1U + 16U + 31U];
        size_t length = 16U;

        memcpy(service_data, config->service_uuid, 16U);
        if (config->service_data != NULL && config->service_data_len > 0U)
        {
            const size_t copy = config->service_data_len <
                                sizeof(service_data) - 16U ?
                                config->service_data_len :
                                sizeof(service_data) - 16U;

            memcpy(service_data + 16U, config->service_data, copy);
            length += copy;
        }
        fields.svc_data_uuid128 = service_data;
        fields.svc_data_uuid128_len = (uint8_t)length;
    }
    result = ble_gap_adv_set_fields(&fields);
    if (result != 0)
    {
        LOG_E("adv fields failed result=%d", result);
        return;
    }
    struct ble_gap_adv_params params;

    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = (uint16_t)(config->interval_ms * 16U / 10U);
    params.itvl_max = params.itvl_min;
    result = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                               &params, NULL, NULL);
    if (result != 0)
    {
        LOG_E("adv start failed result=%d", result);
    }
}

static void _ble_nimble_port_adv_task(void *param)
{
    (void)param;
    for (;;)
    {
        ble_nimble_port_adv_cmd_t cmd;

        if (xQueueReceive(s_port.adv_queue, &cmd, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if (cmd.type == BLE_NIMBLE_PORT_ADV_CMD_START)
        {
            _ble_nimble_port_adv_start_execute(cmd.config);
        }
        else if (cmd.type == BLE_NIMBLE_PORT_ADV_CMD_STOP)
        {
            (void)ble_gap_adv_stop();
        }
        else
        {
            xSemaphoreGive(s_port.adv_exit_semaphore);
            vTaskDelete(NULL);
        }
    }
}

static esp_err_t _ble_nimble_port_production_notify(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    int result;

    if (om == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    result = ble_gatts_notify_custom(conn_handle, value_handle, om);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t _ble_nimble_port_production_indicate(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    int result;

    if (om == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    result = ble_gatts_indicate_custom(conn_handle, value_handle, om);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t _ble_nimble_port_quiesce_adv(void)
{
    if (s_port.adv_task == NULL)
    {
        s_port.quiescing = true;
        return ESP_OK;
    }
    s_port.quiescing = true;
    ble_nimble_port_adv_cmd_t quit;

    quit.type = BLE_NIMBLE_PORT_ADV_CMD_QUIT;
    quit.config = NULL;
    if (xQueueSend(s_port.adv_queue, &quit,
                   pdMS_TO_TICKS(BLE_NIMBLE_PORT_ADV_QUIT_TIMEOUT_MS)) !=
            pdTRUE ||
            xSemaphoreTake(s_port.adv_exit_semaphore,
                           pdMS_TO_TICKS(BLE_NIMBLE_PORT_ADV_QUIT_TIMEOUT_MS)) !=
            pdTRUE)
    {
        s_port.deinit_failed = true;
        s_port.deinit_error = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }
    s_port.adv_task = NULL;
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_rollback_init(
    esp_err_t original_error, bool listener_registered, bool with_queue,
    bool with_task)
{
    if (listener_registered)
    {
        (void)ble_gap_event_listener_unregister(&s_port.listener);
        s_port.listener_registered = false;
    }
    if (with_task && s_port.adv_task != NULL)
    {
        const esp_err_t quiesce = _ble_nimble_port_quiesce_adv();

        if (quiesce != ESP_OK)
        {
            return quiesce;
        }
    }
    if (with_queue && s_port.adv_queue != NULL)
    {
        vQueueDelete(s_port.adv_queue);
        s_port.adv_queue = NULL;
    }
    if (s_port.adv_exit_semaphore != NULL)
    {
        vSemaphoreDelete(s_port.adv_exit_semaphore);
        s_port.adv_exit_semaphore = NULL;
    }
    const esp_err_t cleanup = s_port.nimble_init_attempted
                              ? nimble_port_deinit()
                              : ESP_OK;

    s_port.nimble_init_attempted = false;
    s_port.deinitialized = true;
    if (cleanup != ESP_OK)
    {
        s_port.deinit_failed = true;
        s_port.deinit_error = cleanup;
        return cleanup;
    }
    return original_error;
}

static esp_err_t _ble_nimble_port_init(void)
{
    esp_err_t result;

    if (s_port.deinit_failed)
    {
        return s_port.deinit_error;
    }
    s_port.deinitialized = false;
    s_port.nimble_init_attempted = false;
    s_port.quiescing = false;
    result = ble_event_router_register(_ble_nimble_port_gap_consumer, NULL);
    if (result != ESP_OK)
    {
        return result;
    }
    ble_gap_manager_init();
    s_port.nimble_init_attempted = true;
    result = nimble_port_init();
    if (result != ESP_OK)
    {
        s_port.deinit_failed = true;
        s_port.deinit_error = result;
        return result;
    }
    ble_hs_cfg.reset_cb = _ble_nimble_port_on_reset;
    ble_hs_cfg.sync_cb = _ble_nimble_port_on_sync;
    ble_hs_cfg.gatts_register_cb = _ble_nimble_port_gatts_register;
    ble_hs_cfg.store_status_cb = NULL;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    if (!s_port.listener_registered)
    {
        const int register_result = ble_gap_event_listener_register(
                                        &s_port.listener,
                                        _ble_nimble_port_gap_event, NULL);

        if (register_result != 0 && register_result != BLE_HS_EALREADY)
        {
            return _ble_nimble_port_rollback_init(
                       register_result == BLE_HS_ENOMEM ? ESP_ERR_NO_MEM
                       : ESP_FAIL,
                       false, false, false);
        }
        s_port.listener_registered = true;
    }
    if (s_port.ops == NULL)
    {
        s_port.ops = &s_production_ops;
    }
    if (s_port.adv_queue == NULL)
    {
        s_port.adv_queue = xQueueCreate(BLE_NIMBLE_PORT_ADV_QUEUE_DEPTH,
                                        sizeof(ble_nimble_port_adv_cmd_t));
        if (s_port.adv_queue == NULL)
        {
            return _ble_nimble_port_rollback_init(ESP_ERR_NO_MEM, true, false,
                                                  false);
        }
    }
    if (s_port.adv_exit_semaphore == NULL)
    {
        s_port.adv_exit_semaphore = xSemaphoreCreateBinary();
        if (s_port.adv_exit_semaphore == NULL)
        {
            return _ble_nimble_port_rollback_init(ESP_ERR_NO_MEM, true, true,
                                                  false);
        }
    }
    if (s_port.adv_task == NULL)
    {
        if (xTaskCreate(_ble_nimble_port_adv_task, "ble_adv_ctrl",
                        BLE_NIMBLE_PORT_ADV_TASK_STACK, NULL,
                        BLE_NIMBLE_PORT_ADV_TASK_PRIORITY,
                        &s_port.adv_task) != pdPASS)
        {
            return _ble_nimble_port_rollback_init(ESP_ERR_NO_MEM, true, true,
                                                  false);
        }
    }

    result = _ble_nimble_port_register_database();
    if (result != ESP_OK)
    {
        const esp_err_t cleanup = _ble_nimble_port_rollback_init(
                                      result, true, true, true);

        if (cleanup != ESP_OK && cleanup != result)
        {
            LOG_E("database registration cleanup failed result=%d", cleanup);
        }
        return cleanup;
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

static void _ble_nimble_port_stop_worker(void *param)
{
    (void)param;
    s_port.stop_result = nimble_port_stop();
    xSemaphoreGive(s_port.stop_done_semaphore);
    vTaskDelete(NULL);
}

static esp_err_t _ble_nimble_port_stop(void)
{
    if (s_port.deinit_failed)
    {
        return s_port.deinit_error;
    }
    const esp_err_t quiesce = _ble_nimble_port_quiesce_adv();

    if (quiesce != ESP_OK)
    {
        return quiesce;
    }
    if (s_port.started)
    {
        TaskHandle_t stop_task;

        if (s_port.stop_done_semaphore == NULL)
        {
            s_port.stop_done_semaphore = xSemaphoreCreateBinary();
            if (s_port.stop_done_semaphore == NULL)
            {
                return ESP_ERR_NO_MEM;
            }
        }
        s_port.stop_result = 0;
        if (xTaskCreate(_ble_nimble_port_stop_worker, "ble_stop",
                        BLE_NIMBLE_PORT_ADV_TASK_STACK, NULL,
                        BLE_NIMBLE_PORT_ADV_TASK_PRIORITY,
                        &stop_task) != pdPASS)
        {
            return ESP_ERR_NO_MEM;
        }
        if (xSemaphoreTake(s_port.stop_done_semaphore,
                           pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) !=
                pdTRUE ||
                (s_port.stop_result != 0 && s_port.stop_result != BLE_HS_EALREADY))
        {
            s_port.deinit_failed = true;
            s_port.deinit_error = ESP_ERR_TIMEOUT;
            return ESP_ERR_TIMEOUT;
        }
        if (xSemaphoreTake(s_port.exit_semaphore,
                           pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) !=
                pdTRUE)
        {
            s_port.deinit_failed = true;
            s_port.deinit_error = ESP_ERR_TIMEOUT;
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
    const esp_err_t quiesce = _ble_nimble_port_quiesce_adv();

    if (quiesce != ESP_OK)
    {
        return quiesce;
    }
    if (s_port.adv_queue != NULL)
    {
        vQueueDelete(s_port.adv_queue);
        s_port.adv_queue = NULL;
    }
    if (s_port.adv_exit_semaphore != NULL)
    {
        vSemaphoreDelete(s_port.adv_exit_semaphore);
        s_port.adv_exit_semaphore = NULL;
    }
    if (s_port.listener_registered)
    {
        (void)ble_gap_event_listener_unregister(&s_port.listener);
        s_port.listener_registered = false;
    }
    if (!s_port.deinitialized)
    {
        const esp_err_t result = s_port.nimble_init_attempted
                                 ? nimble_port_deinit()
                                 : ESP_OK;

        s_port.nimble_init_attempted = false;
        s_port.deinitialized = true;
        if (result != ESP_OK)
        {
            s_port.deinit_failed = true;
            s_port.deinit_error = result;
            return result;
        }
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
    if (s_port.stop_done_semaphore != NULL)
    {
        vSemaphoreDelete(s_port.stop_done_semaphore);
        s_port.stop_done_semaphore = NULL;
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

esp_err_t ble_nimble_port_set_ops(const ble_port_ops_t *ops)
{
    if (s_port.started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_port.ops = ops;
    return ESP_OK;
}

esp_err_t ble_nimble_port_register_event_cb(
    ble_port_event_cb_t callback, void *arg)
{
    return ble_event_router_register(callback, arg);
}

esp_err_t ble_nimble_port_unregister_event_cb(
    ble_port_event_cb_t callback, void *arg)
{
    return ble_event_router_unregister(callback, arg);
}

const ble_port_ops_t *ble_nimble_port_get_ops(void)
{
    return s_port.ops != NULL ? s_port.ops : &s_production_ops;
}

const ble_runtime_host_port_t *ble_nimble_port_get(void)
{
    return &s_nimble_port;
}
