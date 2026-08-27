#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "connectivity_manager.h"

static connectivity_manager_status_snapshot_t s_status =
{
    .generation = 1U,
    .state = CONNECTIVITY_MANAGER_STATE_IDLE,
    .failure = CONNECTIVITY_MANAGER_FAILURE_NONE,
    .available = true,
    .radio_available = true,
    .profile_persisted = true,
    .auto_connect = true,
    .profile_revision = CONNECTIVITY_MANAGER_PROFILE_REVISION_INITIAL,
};
static connectivity_manager_scan_snapshot_t s_scan =
{
    .generation = 1U,
};
static connectivity_manager_operation_id_t s_next_operation = 1U;
static esp_err_t s_request_result = ESP_OK;
static char s_saved_ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES + 1U];
static char s_saved_password[CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES + 1U];
static connectivity_manager_credentials_t s_last_saved;

void connectivity_manager_fake_set_request_result(esp_err_t result)
{
    s_request_result = result;
}

void connectivity_manager_fake_reset(void)
{
    s_next_operation = 1U;
    s_request_result = ESP_OK;
    memset(&s_last_saved, 0, sizeof(s_last_saved));
    memset(s_saved_ssid, 0, sizeof(s_saved_ssid));
    memset(s_saved_password, 0, sizeof(s_saved_password));
    memset(&s_status, 0, sizeof(s_status));
    s_status.generation = 1U;
    s_status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    s_status.available = true;
    s_status.radio_available = true;
    s_status.profile_persisted = true;
    s_status.auto_connect = true;
    s_status.profile_revision = CONNECTIVITY_MANAGER_PROFILE_REVISION_INITIAL;
}

void connectivity_manager_fake_set_status(
    const connectivity_manager_status_snapshot_t *snapshot)
{
    if (snapshot != NULL)
    {
        s_status = *snapshot;
    }
}

const connectivity_manager_credentials_t *connectivity_manager_fake_last_saved(
    void)
{
    return s_last_saved.ssid == NULL ? NULL : &s_last_saved;
}

static esp_err_t _admit(connectivity_manager_operation_id_t *operation_id)
{
    if (operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_request_result != ESP_OK)
    {
        return s_request_result;
    }
    *operation_id = s_next_operation++;
    if (*operation_id == 0U)
    {
        *operation_id = s_next_operation++;
    }
    return ESP_OK;
}

esp_err_t connectivity_manager_request_scan(
    connectivity_manager_operation_id_t *operation_id)
{
    return _admit(operation_id);
}

esp_err_t connectivity_manager_get_status(
    connectivity_manager_status_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = s_status;
    return ESP_OK;
}

esp_err_t connectivity_manager_get_scan_snapshot(
    connectivity_manager_scan_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = s_scan;
    return ESP_OK;
}

void connectivity_manager_fake_set_scan(
    const connectivity_manager_scan_snapshot_t *snapshot)
{
    if (snapshot != NULL)
    {
        s_scan = *snapshot;
    }
    else
    {
        memset(&s_scan, 0, sizeof(s_scan));
        s_scan.generation = 1U;
    }
}

esp_err_t connectivity_manager_request_sync_profile(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_client_sync_id_t client_sync_id,
    bool auto_connect,
    connectivity_manager_operation_id_t *operation_id)
{
    if (credentials == NULL || credentials->ssid == NULL ||
            credentials->password == NULL || client_sync_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_status.auto_connect = auto_connect;
    s_status.applied_client_sync_id = client_sync_id;
    return _admit(operation_id);
}

esp_err_t connectivity_manager_request_disconnect(
    connectivity_manager_operation_id_t *operation_id)
{
    return _admit(operation_id);
}

esp_err_t connectivity_manager_request_reconnect_saved(
    connectivity_manager_operation_id_t *operation_id)
{
    return _admit(operation_id);
}

esp_err_t connectivity_manager_request_forget(
    connectivity_manager_operation_id_t *operation_id)
{
    return _admit(operation_id);
}

esp_err_t connectivity_manager_request_save_profile(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_operation_id_t *operation_id)
{
    if (credentials == NULL || credentials->ssid == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_saved_ssid, 0, sizeof(s_saved_ssid));
    memset(s_saved_password, 0, sizeof(s_saved_password));
    if (credentials->ssid_length > CONNECTIVITY_MANAGER_SSID_MAX_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_saved_ssid, credentials->ssid, credentials->ssid_length);
    if (credentials->password != NULL && credentials->password_length > 0U &&
            credentials->password_length <=
            CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES)
    {
        memcpy(s_saved_password, credentials->password,
               credentials->password_length);
    }
    s_last_saved.ssid = s_saved_ssid;
    s_last_saved.ssid_length = credentials->ssid_length;
    s_last_saved.password = s_saved_password;
    s_last_saved.password_length = credentials->password_length;
    s_last_saved.security = credentials->security;
    return _admit(operation_id);
}

esp_err_t connectivity_manager_set_auto_connect(
    bool enabled, connectivity_manager_operation_id_t *operation_id)
{
    s_status.auto_connect = enabled;
    return _admit(operation_id);
}

bool connectivity_manager_is_available(void)
{
    return true;
}

/* Keep the rest of the public owner API linkable if this fake is reused. */
esp_err_t connectivity_manager_init(const connectivity_manager_config_t *config)
{
    return config == NULL ? ESP_ERR_INVALID_ARG : ESP_OK;
}

esp_err_t connectivity_manager_clear_persisted_profile(void)
{
    return ESP_OK;
}

esp_err_t connectivity_manager_deinit(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t connectivity_manager_suspend(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t connectivity_manager_resume(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t connectivity_manager_request_connect(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_operation_id_t *operation_id)
{
    return credentials == NULL ? ESP_ERR_INVALID_ARG : _admit(operation_id);
}

esp_err_t connectivity_manager_cancel(
    connectivity_manager_operation_id_t operation_id)
{
    return operation_id == 0U ? ESP_ERR_INVALID_ARG : ESP_OK;
}
