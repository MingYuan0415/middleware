#include "system_pm_fakes.h"

#include <string.h>

#include "esp_pm.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"

#define SYSTEM_PM_FAKES_MAX_GPIO 49U

struct esp_pm_lock
{
    int marker;
};

typedef struct fake_lock_state
{
    bool live;
    unsigned refs;
} fake_lock_state_t;

static struct esp_pm_lock s_lock_marker;
static fake_lock_state_t s_lock;
static esp_err_t s_fail_lock_create;
static esp_err_t s_fail_lock_acquire;
static esp_err_t s_fail_lock_release;
static esp_err_t s_fail_lock_delete;
static esp_err_t s_enable_ext1_result;
static esp_err_t s_light_sleep_result;
static uint32_t s_wakeup_causes;
static uint64_t s_ext1_status;
static uint64_t s_last_ext1_mask;
static int s_last_ext1_mode = -1;
static unsigned s_acquire_count;
static unsigned s_release_count;
static unsigned s_delete_count;
static unsigned s_sleep_count;
static unsigned s_ext1_enable_count;
static unsigned s_ext1_disable_count;
static bool s_rtc_gpios[SYSTEM_PM_FAKES_MAX_GPIO];
static unsigned s_rtc_gpio_count;

void system_pm_fakes_reset(void)
{
    memset(&s_lock, 0, sizeof(s_lock));
    s_fail_lock_create = ESP_OK;
    s_fail_lock_acquire = ESP_OK;
    s_fail_lock_release = ESP_OK;
    s_fail_lock_delete = ESP_OK;
    s_enable_ext1_result = ESP_OK;
    s_light_sleep_result = ESP_OK;
    s_wakeup_causes = 0U;
    s_ext1_status = 0U;
    s_last_ext1_mask = 0U;
    s_last_ext1_mode = -1;
    s_acquire_count = 0U;
    s_release_count = 0U;
    s_delete_count = 0U;
    s_sleep_count = 0U;
    s_ext1_enable_count = 0U;
    s_ext1_disable_count = 0U;
    s_rtc_gpio_count = 0U;
    memset(s_rtc_gpios, 0, sizeof(s_rtc_gpios));
}

void system_pm_fakes_fail_lock_create(esp_err_t result)
{
    s_fail_lock_create = result;
}

void system_pm_fakes_fail_lock_acquire(esp_err_t result)
{
    s_fail_lock_acquire = result;
}

void system_pm_fakes_fail_lock_release(esp_err_t result)
{
    s_fail_lock_release = result;
}

void system_pm_fakes_fail_lock_delete(esp_err_t result)
{
    s_fail_lock_delete = result;
}

void system_pm_fakes_set_enable_ext1_result(esp_err_t result)
{
    s_enable_ext1_result = result;
}

void system_pm_fakes_set_light_sleep_result(esp_err_t result)
{
    s_light_sleep_result = result;
}

void system_pm_fakes_set_wakeup(uint32_t causes, uint64_t ext1_status)
{
    s_wakeup_causes = causes;
    s_ext1_status = ext1_status;
}

unsigned system_pm_fakes_live_locks(void)
{
    return s_lock.live ? 1U : 0U;
}

unsigned system_pm_fakes_acquire_count(void)
{
    return s_acquire_count;
}

unsigned system_pm_fakes_release_count(void)
{
    return s_release_count;
}

unsigned system_pm_fakes_delete_count(void)
{
    return s_delete_count;
}

unsigned system_pm_fakes_sleep_count(void)
{
    return s_sleep_count;
}

uint64_t system_pm_fakes_last_ext1_mask(void)
{
    return s_last_ext1_mask;
}

int system_pm_fakes_last_ext1_mode(void)
{
    return s_last_ext1_mode;
}

unsigned system_pm_fakes_ext1_enable_count(void)
{
    return s_ext1_enable_count;
}

unsigned system_pm_fakes_ext1_disable_count(void)
{
    return s_ext1_disable_count;
}

unsigned system_pm_fakes_rtc_gpio_count(void)
{
    return s_rtc_gpio_count;
}

void system_pm_fakes_set_rtc_gpios(const int *gpios, size_t count)
{
    s_rtc_gpio_count = 0U;
    memset(s_rtc_gpios, 0, sizeof(s_rtc_gpios));
    for (size_t index = 0U; index < count; ++index)
    {
        const int gpio = gpios[index];
        if (gpio >= 0 && (size_t)gpio < SYSTEM_PM_FAKES_MAX_GPIO)
        {
            s_rtc_gpios[gpio] = true;
            ++s_rtc_gpio_count;
        }
    }
}

bool system_pm_fakes_rtc_gpio_is_valid(int gpio_num)
{
    return gpio_num >= 0 && (size_t)gpio_num < SYSTEM_PM_FAKES_MAX_GPIO &&
           s_rtc_gpios[gpio_num];
}

esp_err_t esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg,
                             const char *name,
                             esp_pm_lock_handle_t *out_handle)
{
    (void)arg;
    (void)name;
    (void)lock_type;
    if (out_handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_fail_lock_create != ESP_OK)
    {
        const esp_err_t result = s_fail_lock_create;

        s_fail_lock_create = ESP_OK;
        return result;
    }
    s_lock.live = true;
    s_lock.refs = 0U;
    *out_handle = &s_lock_marker;
    return ESP_OK;
}

esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle)
{
    if (handle != &s_lock_marker || !s_lock.live)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ++s_acquire_count;
    if (s_fail_lock_acquire != ESP_OK)
    {
        const esp_err_t result = s_fail_lock_acquire;

        s_fail_lock_acquire = ESP_OK;
        return result;
    }
    ++s_lock.refs;
    return ESP_OK;
}

esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle)
{
    if (handle != &s_lock_marker || !s_lock.live)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ++s_release_count;
    if (s_fail_lock_release != ESP_OK)
    {
        const esp_err_t result = s_fail_lock_release;

        s_fail_lock_release = ESP_OK;
        return result;
    }
    if (s_lock.refs == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    --s_lock.refs;
    return ESP_OK;
}

esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t handle)
{
    if (handle != &s_lock_marker || !s_lock.live)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ++s_delete_count;
    if (s_fail_lock_delete != ESP_OK)
    {
        const esp_err_t result = s_fail_lock_delete;

        s_fail_lock_delete = ESP_OK;
        return result;
    }
    s_lock.live = false;
    return ESP_OK;
}

esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t io_mask,
        esp_sleep_ext1_wakeup_mode_t level_mode)
{
    ++s_ext1_enable_count;
    s_last_ext1_mask = io_mask;
    s_last_ext1_mode = (int)level_mode;
    return s_enable_ext1_result;
}

esp_err_t esp_sleep_disable_ext1_wakeup_io(uint64_t io_mask)
{
    (void)io_mask;
    ++s_ext1_disable_count;
    return ESP_OK;
}

esp_err_t esp_light_sleep_start(void)
{
    ++s_sleep_count;
    return s_light_sleep_result;
}

uint32_t esp_sleep_get_wakeup_causes(void)
{
    return s_wakeup_causes;
}

uint64_t esp_sleep_get_ext1_wakeup_status(void)
{
    return s_ext1_status;
}
