#ifndef __HOST_LIFECYCLE_NPL_H__
#define __HOST_LIFECYCLE_NPL_H__
struct ble_npl_event
{
    void *event;
};
typedef void ble_npl_event_fn(struct ble_npl_event *event);
void ble_npl_event_init(struct ble_npl_event *event, ble_npl_event_fn *fn, void *arg);
void ble_npl_event_deinit(struct ble_npl_event *event);
#endif
