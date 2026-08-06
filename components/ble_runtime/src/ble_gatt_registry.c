#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_gatt_registry.h"

#define DBG_TAG "ble_gatt_registry"
#define DBG_LVL DBG_WARN
#include "mt_log.h"


typedef struct ble_gatt_registry
{
    const ble_gatt_registry_service_t *services[
        BLE_GATT_REGISTRY_MAX_SERVICES];
    size_t service_count;
    const ble_gatt_registry_characteristic_t *characteristics[
        BLE_GATT_REGISTRY_MAX_CHARACTERISTICS];
    size_t characteristic_count;
    uint16_t handle_map[BLE_GATT_REGISTRY_MAX_CHARACTERISTICS];
    bool sealed;
} ble_gatt_registry_t;

static ble_gatt_registry_t s_registry;

static bool _ble_gatt_registry_uuid_equal(
    const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, 16U) == 0;
}

static bool _ble_gatt_registry_service_uuid_exists(
    const uint8_t *uuid)
{
    for (size_t i = 0U; i < s_registry.service_count; ++i)
    {
        if (_ble_gatt_registry_uuid_equal(
                    s_registry.services[i]->uuid, uuid))
        {
            return true;
        }
    }
    return false;
}

static bool _ble_gatt_registry_characteristic_uuid_exists(
    const uint8_t *uuid)
{
    for (size_t i = 0U; i < s_registry.characteristic_count; ++i)
    {
        if (_ble_gatt_registry_uuid_equal(
                    s_registry.characteristics[i]->uuid, uuid))
        {
            return true;
        }
    }
    return false;
}

static bool _ble_gatt_registry_handle_exists(uint16_t value_handle)
{
    for (size_t i = 0U; i < s_registry.characteristic_count; ++i)
    {
        if (s_registry.handle_map[i] == value_handle)
        {
            return true;
        }
    }
    return false;
}

void ble_gatt_registry_init(void)
{
    memset(&s_registry, 0, sizeof(s_registry));
}

static esp_err_t _ble_gatt_registry_validate_service(
    const ble_gatt_registry_service_t *service)
{
    if (service == NULL || service->uuid == NULL ||
            service->characteristics == NULL ||
            service->characteristic_count == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_registry.sealed)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_registry.service_count >= BLE_GATT_REGISTRY_MAX_SERVICES)
    {
        return ESP_ERR_NO_MEM;
    }
    if (_ble_gatt_registry_service_uuid_exists(service->uuid))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_registry.characteristic_count > BLE_GATT_REGISTRY_MAX_CHARACTERISTICS ||
            service->characteristic_count >
            BLE_GATT_REGISTRY_MAX_CHARACTERISTICS -
            s_registry.characteristic_count)
    {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0U; i < service->characteristic_count; ++i)
    {
        const ble_gatt_registry_characteristic_t *characteristic =
            &service->characteristics[i];

        if (characteristic->uuid == NULL || characteristic->access_cb == NULL)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (_ble_gatt_registry_characteristic_uuid_exists(
                    characteristic->uuid))
        {
            return ESP_ERR_INVALID_STATE;
        }
        for (size_t j = 0U; j < i; ++j)
        {
            if (_ble_gatt_registry_uuid_equal(
                        service->characteristics[j].uuid,
                        characteristic->uuid))
            {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    return ESP_OK;
}

esp_err_t ble_gatt_registry_register(
    const ble_gatt_registry_service_t *service)
{
    const esp_err_t validation = _ble_gatt_registry_validate_service(service);

    if (validation != ESP_OK)
    {
        return validation;
    }
    for (size_t i = 0U; i < service->characteristic_count; ++i)
    {
        s_registry.characteristics[s_registry.characteristic_count] =
            &service->characteristics[i];
        s_registry.characteristic_count++;
    }
    s_registry.services[s_registry.service_count] = service;
    s_registry.service_count++;
    return ESP_OK;
}

esp_err_t ble_gatt_registry_seal(void)
{
    if (s_registry.sealed)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_registry.sealed = true;
    return ESP_OK;
}

bool ble_gatt_registry_is_sealed(void)
{
    return s_registry.sealed;
}

void ble_gatt_registry_clear_handles(void)
{
    memset(s_registry.handle_map, 0, sizeof(s_registry.handle_map));
}

esp_err_t ble_gatt_registry_assign_handle(
    const uint8_t *uuid, uint16_t value_handle)
{
    if (uuid == NULL || value_handle == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < s_registry.characteristic_count; ++i)
    {
        if (!_ble_gatt_registry_uuid_equal(
                    s_registry.characteristics[i]->uuid, uuid))
        {
            continue;
        }
        if (s_registry.handle_map[i] == value_handle)
        {
            return ESP_OK;
        }
        if (_ble_gatt_registry_handle_exists(value_handle))
        {
            return ESP_ERR_INVALID_STATE;
        }
        s_registry.handle_map[i] = value_handle;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_gatt_registry_get_assigned_handle(
    const uint8_t *uuid, uint16_t *out)
{
    if (uuid == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out = 0U;
    for (size_t i = 0U; i < s_registry.characteristic_count; ++i)
    {
        if (_ble_gatt_registry_uuid_equal(
                    s_registry.characteristics[i]->uuid, uuid))
        {
            if (s_registry.handle_map[i] == 0U)
            {
                return ESP_ERR_INVALID_STATE;
            }
            *out = s_registry.handle_map[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_gatt_registry_lookup_by_handle(
    uint16_t value_handle,
    const ble_gatt_registry_characteristic_t **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (value_handle == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < s_registry.characteristic_count; ++i)
    {
        if (s_registry.handle_map[i] == value_handle)
        {
            *out = s_registry.characteristics[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_gatt_registry_lookup_by_uuid(
    const uint8_t *uuid,
    const ble_gatt_registry_characteristic_t **out)
{
    if (uuid == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    for (size_t i = 0U; i < s_registry.characteristic_count; ++i)
    {
        if (_ble_gatt_registry_uuid_equal(
                    s_registry.characteristics[i]->uuid, uuid))
        {
            *out = s_registry.characteristics[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_gatt_registry_get_characteristic(
    size_t index, const ble_gatt_registry_characteristic_t **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (index >= s_registry.characteristic_count)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_registry.characteristics[index];
    return ESP_OK;
}

esp_err_t ble_gatt_registry_get_service(
    size_t index, const ble_gatt_registry_service_t **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (index >= s_registry.service_count)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_registry.services[index];
    return ESP_OK;
}

bool ble_gatt_registry_admission_requires_encryption(
    ble_gatt_registry_admission_t admission)
{
    return admission >= BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND;
}
