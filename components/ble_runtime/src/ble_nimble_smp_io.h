#ifndef __BLE_NIMBLE_SMP_IO_H__
#define __BLE_NIMBLE_SMP_IO_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ble_nimble_smp_reply_phase
{
    BLE_NIMBLE_SMP_REPLY_IDLE = 0,
    BLE_NIMBLE_SMP_REPLY_QUEUED,
    BLE_NIMBLE_SMP_REPLY_EXECUTING,
    BLE_NIMBLE_SMP_REPLY_DONE,
    BLE_NIMBLE_SMP_REPLY_CANCELLED,
} ble_nimble_smp_reply_phase_t;

typedef struct ble_nimble_smp_io_tuple
{
    uint32_t generation;
    uint16_t conn_handle;
    uint64_t token;
} ble_nimble_smp_io_tuple_t;

typedef struct ble_nimble_smp_io_reply
{
    uint64_t seq;
    ble_nimble_smp_io_tuple_t tuple;
    bool accept;
    uint64_t deadline_us;
    ble_nimble_smp_reply_phase_t phase;
    esp_err_t result;
    bool waiter_attached;
    bool restore_allowed;
} ble_nimble_smp_io_reply_t;

typedef struct ble_nimble_smp_io_ops
{
    bool (*is_host_current)(void *arg);
    void (*wake_owner)(void *arg);
    uint64_t (*now_us)(void *arg);
    esp_err_t (*current_connection)(uint32_t *generation,
                                    uint16_t *conn_handle, void *arg);
    bool (*host_ready)(void *arg);
    int (*inject)(uint16_t conn_handle, bool accept, void *arg);
    esp_err_t (*publish_confirmation)(uint32_t passkey, uint64_t token,
                                      void *arg);
    void (*retire_confirmation)(uint64_t token, bool accept, void *arg);
    void *arg;
} ble_nimble_smp_io_ops_t;

typedef struct ble_nimble_smp_io
{
    /* This header is intentionally visible to the port for static storage. */
    ble_nimble_smp_io_ops_t ops;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t ack;
    StaticSemaphore_t lock_storage;
    StaticSemaphore_t ack_storage;
    bool sync_ready;
    bool initialized;
    bool accepting;
    bool open_requested;
    bool event_live;
    bool draining;
    bool offer_pending;
    bool offer_token_active;
    uint32_t offer_passkey;
    ble_nimble_smp_io_tuple_t offer;
    ble_nimble_smp_io_reply_t reply;
    ble_nimble_smp_io_tuple_t cancel;
    bool cancel_pending;
    uint64_t next_seq;
} ble_nimble_smp_io_t;

esp_err_t ble_nimble_smp_io_init(ble_nimble_smp_io_t *io,
                                 const ble_nimble_smp_io_ops_t *ops);
void ble_nimble_smp_io_open(ble_nimble_smp_io_t *io);
esp_err_t ble_nimble_smp_io_offer(ble_nimble_smp_io_t *io,
                                  uint32_t generation,
                                  uint16_t conn_handle,
                                  uint64_t token,
                                  uint32_t passkey);
esp_err_t ble_nimble_smp_io_reply(ble_nimble_smp_io_t *io,
                                  uint64_t token,
                                  bool accept,
                                  uint32_t timeout_ms);
void ble_nimble_smp_io_cancel(ble_nimble_smp_io_t *io);
void ble_nimble_smp_io_forget_connection(ble_nimble_smp_io_t *io,
        uint32_t generation,
        uint16_t conn_handle);
void ble_nimble_smp_io_close(ble_nimble_smp_io_t *io);
bool ble_nimble_smp_io_claim_event(ble_nimble_smp_io_t *io);
void ble_nimble_smp_io_run_host(ble_nimble_smp_io_t *io);
bool ble_nimble_smp_io_pending(ble_nimble_smp_io_t *io);
void ble_nimble_smp_io_host_exited(ble_nimble_smp_io_t *io);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NIMBLE_SMP_IO_H__ */
