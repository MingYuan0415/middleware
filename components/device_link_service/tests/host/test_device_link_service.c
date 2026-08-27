#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "device_link_service.h"

static void test_status_exposes_numeric_comparison(void)
{
    device_link_service_status_t status;

    memset(&status, 0, sizeof(status));
    status.pending_confirmation = true;
    status.numeric_comparison = 123456U;
    status.confirmation_token = 7U;
    assert(status.pending_confirmation);
    assert(status.numeric_comparison == 123456U);
    assert(status.confirmation_token == 7U);
    assert(offsetof(device_link_service_status_t, numeric_comparison) > 0U);
}

int main(void)
{
    test_status_exposes_numeric_comparison();
    return 0;
}
