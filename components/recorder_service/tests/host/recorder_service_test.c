#include "recorder_service.h"

#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int64_t s_now_us;

int64_t esp_timer_get_time(void)
{
    s_now_us += 1000;
    return s_now_us;
}

static void _remove_directory_contents(const char *directory)
{
    DIR *dir = opendir(directory);
    if (dir == NULL)
    {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        char path[256];
        const int length = snprintf(path, sizeof(path), "%s/%s", directory,
                                    entry->d_name);
        if (length > 0 && (size_t)length < sizeof(path))
        {
            (void)unlink(path);
        }
    }
    (void)closedir(dir);
}

int main(void)
{
    char directory[] = "/tmp/mt-recorder-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    const recorder_service_config_t config =
    {
        .directory = directory,
        .max_duration_seconds = 60U,
        .minimum_free_bytes = 1U,
    };
    assert(recorder_service_init(&config) == ESP_OK);
    assert(recorder_service_start() == ESP_OK);
    recorder_service_snapshot_t snapshot;
    assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RECORDER_SERVICE_RECORDING);
    assert(recorder_service_start() == ESP_ERR_INVALID_STATE);
    assert(recorder_service_pause() == ESP_OK);
    assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RECORDER_SERVICE_PAUSED);
    assert(recorder_service_resume() == ESP_OK);
    assert(recorder_service_stop() == ESP_OK);
    assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RECORDER_SERVICE_IDLE);
    recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES];
    size_t count = 0U;
    assert(recorder_service_list(files, RECORDER_SERVICE_MAX_FILES,
                                 &count) == ESP_OK);
    assert(count == 1U);
    assert(strstr(files[0].name, ".wav") != NULL);
    assert(recorder_service_play(files[0].name) == ESP_OK);
    assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RECORDER_SERVICE_IDLE);
    assert(snapshot.bytes == 0U);
    const esp_err_t missing_result = recorder_service_play("/tmp/not-owned.wav");
    assert(missing_result == ESP_ERR_INVALID_ARG ||
           missing_result == ESP_ERR_NOT_FOUND || missing_result == ESP_FAIL);
    assert(recorder_service_delete(files[0].name) == ESP_OK);
    assert(recorder_service_deinit() == ESP_OK);
    assert(recorder_service_init(&config) == ESP_OK);
    assert(recorder_service_start() == ESP_OK);
    assert(recorder_service_deinit() == ESP_OK);
    assert(recorder_service_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    _remove_directory_contents(directory);
    assert(rmdir(directory) == 0);
    return 0;
}
