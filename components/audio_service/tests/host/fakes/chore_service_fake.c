#include "audio_service_fakes.h"

#include "chore_service.h"

#include <stddef.h>

/* Host fake: runs the job inline so the persistence assertions observe the
 * store before chore_service_submit() returns. The firmware worker is a
 * separate internal-RAM task; that distinction is not reproducible on the
 * host, but the job body and the queue contract are. */

static unsigned s_submit_count;
static bool s_fail;
static esp_err_t s_fail_error;

void audio_service_fakes_chore_reset(void)
{
    s_submit_count = 0U;
    s_fail = false;
    s_fail_error = ESP_OK;
}

unsigned audio_service_fakes_chore_submit_count(void)
{
    return s_submit_count;
}

void audio_service_fakes_chore_fail(esp_err_t error)
{
    s_fail = true;
    s_fail_error = error;
}

esp_err_t chore_service_submit(const chore_service_job_t *job,
                               chore_service_handle_t *handle)
{
    if (job == NULL || job->run == NULL || handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ++s_submit_count;
    if (s_fail)
    {
        s_fail = false;
        return s_fail_error;
    }
    job->run(NULL, job->arg);
    if (job->release != NULL)
    {
        job->release(job->arg);
    }
    *handle = (chore_service_handle_t)
    {
        .slot = 0U,
        .generation = 1U,
    };
    return ESP_OK;
}
