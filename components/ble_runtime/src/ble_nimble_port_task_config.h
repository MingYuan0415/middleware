#ifndef __BLE_NIMBLE_PORT_TASK_CONFIG_H__
#define __BLE_NIMBLE_PORT_TASK_CONFIG_H__

/* The deadline sweep can synchronously retire a Security 2 flow and retain
 * provisional-bond cleanup. That path crosses the TX scheduler, link service,
 * and NimBLE store helpers, so the owner needs substantially more than the
 * 1 KiB suitable for a timer-only task. */
#define BLE_NIMBLE_PORT_LINK_TIMER_STACK_BYTES 4096U

#endif /* __BLE_NIMBLE_PORT_TASK_CONFIG_H__ */
