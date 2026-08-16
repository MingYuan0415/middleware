#include <stddef.h>
#include <string.h>

#include "device_link_core.h"

static const device_link_tlv_schema_t s_empty_schema =
{
    .fields = NULL,
    .field_count = 0U,
    .maximum_encoded_bytes = 0U,
};

static const uint64_t s_authorization_states[] = {1U, 2U, 3U, 4U, 5U};
static const uint64_t s_operation_states[] = {1U, 2U, 3U, 4U, 5U};
static const uint64_t s_status_values[] =
{
    DEVICE_LINK_STATUS_OK,
    DEVICE_LINK_STATUS_MALFORMED_FRAME,
    DEVICE_LINK_STATUS_UNSUPPORTED_VERSION,
    DEVICE_LINK_STATUS_UNSUPPORTED_OPERATION,
    DEVICE_LINK_STATUS_UNSUPPORTED_CAPABILITY,
    DEVICE_LINK_STATUS_UNAUTHENTICATED,
    DEVICE_LINK_STATUS_PERMISSION_DENIED,
    DEVICE_LINK_STATUS_CONFIRMATION_REQUIRED,
    DEVICE_LINK_STATUS_INVALID_ARGUMENT,
    DEVICE_LINK_STATUS_BUSY,
    DEVICE_LINK_STATUS_NOT_FOUND,
    DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED,
    DEVICE_LINK_STATUS_CONFLICT,
    DEVICE_LINK_STATUS_UNAVAILABLE,
    DEVICE_LINK_STATUS_STORAGE,
    DEVICE_LINK_STATUS_INTERNAL,
};

static const device_link_tlv_field_rule_t s_protocol_version_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = UINT8_MAX
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = UINT8_MAX
    },
};
static const device_link_tlv_schema_t s_protocol_version_schema =
{
    .fields = s_protocol_version_fields, .field_count = 2U,
    .maximum_encoded_bytes = 20U,
};

static const device_link_tlv_field_rule_t s_framing_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = UINT8_MAX
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = UINT8_MAX
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 517U
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 4096U
    },
    {
        .id = 5U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 1024U
    },
    {
        .id = 6U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 3072U
    },
};
static const device_link_tlv_schema_t s_framing_schema =
{
    .fields = s_framing_fields, .field_count = 6U,
    .maximum_encoded_bytes = 64U,
};

static const device_link_tlv_field_rule_t s_security_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 16U
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 1U
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 2U
    },
    {
        .id = 5U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 1U
    },
    {
        .id = 6U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U
    },
    {
        .id = 7U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U
    },
    {
        .id = 8U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U
    },
};
static const device_link_tlv_schema_t s_security_schema =
{
    .fields = s_security_fields, .field_count = 8U,
    .maximum_encoded_bytes = 64U,
};

static const device_link_tlv_field_rule_t s_method_descriptor_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 254U
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = UINT16_MAX
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_BOOL,
        .maximum_unsigned = 1U
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 3072U
    },
    {
        .id = 5U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 3072U
    },
};
static const device_link_tlv_schema_t s_method_descriptor_schema =
{
    .fields = s_method_descriptor_fields, .field_count = 5U,
    .maximum_encoded_bytes = 64U,
};

static const device_link_tlv_field_rule_t s_domain_descriptor_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 254U
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = UINT8_MAX
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = UINT8_MAX
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REPEATED | DEVICE_LINK_TLV_RULE_MESSAGE,
        .maximum_count = 12U, .nested = &s_method_descriptor_schema
    },
};
static const device_link_tlv_schema_t s_domain_descriptor_schema =
{
    .fields = s_domain_descriptor_fields, .field_count = 4U,
    .maximum_encoded_bytes = 768U,
};

static const device_link_tlv_field_rule_t s_manifest_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_MESSAGE,
        .nested = &s_protocol_version_schema
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_MESSAGE,
        .nested = &s_protocol_version_schema
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_MESSAGE,
        .nested = &s_framing_schema
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_MESSAGE,
        .nested = &s_security_schema
    },
    {
        .id = 5U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_REPEATED |
        DEVICE_LINK_TLV_RULE_MESSAGE,
        .maximum_count = 4U, .nested = &s_domain_descriptor_schema
    },
};
static const device_link_tlv_schema_t s_manifest_schema =
{
    .fields = s_manifest_fields, .field_count = 5U,
    .maximum_encoded_bytes = 1005U,
};

static const device_link_tlv_field_rule_t s_operation_status_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_NONZERO
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 254U
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 254U
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 5U,
        .enum_values = s_operation_states, .enum_count = 5U
    },
    {
        .id = 5U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 16U,
        .enum_values = s_status_values, .enum_count = 16U
    },
    {
        .id = 6U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .maximum_bytes = 3000U
    },
};
static const device_link_tlv_schema_t s_operation_status_schema =
{
    .fields = s_operation_status_fields, .field_count = 6U,
    .maximum_encoded_bytes = 3072U,
};

static const device_link_tlv_schema_t s_operation_summary_schema =
{
    .fields = s_operation_status_fields, .field_count = 5U,
    .maximum_encoded_bytes = 128U,
};

static const device_link_tlv_field_rule_t s_snapshot_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_NONZERO
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
        .minimum_bytes = 16U, .maximum_bytes = 16U
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REPEATED | DEVICE_LINK_TLV_RULE_MESSAGE,
        .maximum_count = 4U, .nested = &s_operation_summary_schema
    },
};
static const device_link_tlv_schema_t s_snapshot_schema =
{
    .fields = s_snapshot_fields, .field_count = 3U,
    .maximum_encoded_bytes = 1005U,
};

static const device_link_tlv_field_rule_t s_auth_prepare_fields[] =
{
    {
        .id = 1U,
        .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED |
        DEVICE_LINK_TLV_RULE_REPEATED,
        .maximum_count = 16U,
        .maximum_unsigned = UINT16_MAX,
    },
};

static const device_link_tlv_schema_t s_auth_prepare_schema =
{
    .fields = s_auth_prepare_fields,
    .field_count = 1U,
    .maximum_encoded_bytes = 80U,
};

static const device_link_tlv_field_rule_t s_auth_prepare_response_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_NONZERO
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED |
        DEVICE_LINK_TLV_RULE_NONZERO,
        .minimum_bytes = 16U,
        .maximum_bytes = 16U
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .minimum_bytes = 16U,
        .maximum_bytes = 16U
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
        .minimum_unsigned = 1U, .maximum_unsigned = 120000U
    },
    {
        .id = 5U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_REPEATED,
        .minimum_unsigned = 1U, .maximum_count = 16U,
        .maximum_unsigned = UINT16_MAX
    },
};
static const device_link_tlv_schema_t s_auth_prepare_response_schema =
{
    .fields = s_auth_prepare_response_fields, .field_count = 5U,
    .maximum_encoded_bytes = 256U,
};

static const device_link_tlv_field_rule_t s_auth_commit_fields[] =
{
    {
        .id = 1U,
        .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED |
        DEVICE_LINK_TLV_RULE_NONZERO,
    },
    {
        .id = 2U,
        .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED |
        DEVICE_LINK_TLV_RULE_NONZERO,
        .minimum_bytes = 16U,
        .maximum_bytes = 16U,
    },
};

static const device_link_tlv_schema_t s_auth_commit_schema =
{
    .fields = s_auth_commit_fields,
    .field_count = 2U,
    .maximum_encoded_bytes = 64U,
};

static const device_link_tlv_field_rule_t s_get_authorization_fields[] =
{
    {
        .id = 1U,
        .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED |
        DEVICE_LINK_TLV_RULE_NONZERO,
        .minimum_bytes = 16U,
        .maximum_bytes = 16U,
    },
};

static const device_link_tlv_schema_t s_get_authorization_schema =
{
    .fields = s_get_authorization_fields,
    .field_count = 1U,
    .maximum_encoded_bytes = 20U,
};

static const device_link_tlv_field_rule_t s_authorization_result_fields[] =
{
    {
        .id = 1U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED |
        DEVICE_LINK_TLV_RULE_NONZERO,
        .minimum_bytes = 16U, .maximum_bytes = 16U
    },
    {
        .id = 2U, .wire_type = DEVICE_LINK_TLV_LENGTH,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED |
        DEVICE_LINK_TLV_RULE_NONZERO,
        .minimum_bytes = 16U, .maximum_bytes = 16U
    },
    {
        .id = 3U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED, .maximum_unsigned = 5U,
        .enum_values = s_authorization_states, .enum_count = 5U
    },
    {
        .id = 4U, .wire_type = DEVICE_LINK_TLV_UNSIGNED,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED | DEVICE_LINK_TLV_RULE_REPEATED,
        .maximum_count = 16U, .maximum_unsigned = UINT16_MAX
    },
    {
        .id = 5U, .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_NONZERO
    },
};
static const device_link_tlv_schema_t s_authorization_result_schema =
{
    .fields = s_authorization_result_fields, .field_count = 5U,
    .maximum_encoded_bytes = 256U,
};

static const device_link_tlv_field_rule_t s_operation_fields[] =
{
    {
        .id = 1U,
        .wire_type = DEVICE_LINK_TLV_FIXED64,
        .flags = DEVICE_LINK_TLV_RULE_REQUIRED |
        DEVICE_LINK_TLV_RULE_NONZERO,
    },
};

static const device_link_tlv_schema_t s_operation_schema =
{
    .fields = s_operation_fields,
    .field_count = 1U,
    .maximum_encoded_bytes = 32U,
};

static device_link_status_t _method(
    const device_link_request_context_t *context,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len,
    void *arg)
{
    device_link_core_t *core = arg;

    if (core == NULL || core->callbacks.method == NULL)
    {
        return DEVICE_LINK_STATUS_UNAVAILABLE;
    }
    return core->callbacks.method(context, request, request_len, response,
                                  response_capacity, response_len,
                                  core->callbacks.arg);
}

static const device_link_tlv_schema_t *const s_request_schemas[] =
{
    &s_empty_schema, &s_empty_schema, &s_auth_prepare_schema,
    &s_auth_commit_schema, &s_get_authorization_schema,
    &s_operation_schema,
    &s_operation_schema,
};
static const device_link_tlv_schema_t *const s_response_schemas[] =
{
    &s_manifest_schema, &s_snapshot_schema,
    &s_auth_prepare_response_schema, &s_authorization_result_schema,
    &s_authorization_result_schema, &s_operation_status_schema,
    &s_operation_status_schema,
};

#ifdef UNIT_TEST_HOST
const device_link_tlv_schema_t *device_link_core_test_request_schema(
    uint8_t method_id)
{
    if (method_id < 1U || method_id > 7U)
    {
        return NULL;
    }
    return s_request_schemas[method_id - 1U];
}

const device_link_tlv_schema_t *device_link_core_test_response_schema(
    uint8_t method_id)
{
    if (method_id < 1U || method_id > 7U)
    {
        return NULL;
    }
    return s_response_schemas[method_id - 1U];
}
#endif

esp_err_t device_link_core_init(
    device_link_core_t *core, const device_link_core_callbacks_t *callbacks)
{
    if (core == NULL || callbacks == NULL || callbacks->method == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(core, 0, sizeof(*core));
    core->callbacks = *callbacks;
    static const uint16_t permissions[] =
    {
        DEVICE_LINK_PERMISSION_CORE_READ,
        DEVICE_LINK_PERMISSION_CORE_READ,
        DEVICE_LINK_PERMISSION_CORE_BIND,
        DEVICE_LINK_PERMISSION_CORE_BIND,
        DEVICE_LINK_PERMISSION_CORE_READ,
        DEVICE_LINK_PERMISSION_CORE_READ,
        DEVICE_LINK_PERMISSION_CORE_OPERATE,
    };
    static const uint8_t flags[] =
    {
        DEVICE_LINK_METHOD_BOOTSTRAP,
        DEVICE_LINK_METHOD_BOOTSTRAP,
        DEVICE_LINK_METHOD_BOOTSTRAP,
        DEVICE_LINK_METHOD_BOOTSTRAP,
        DEVICE_LINK_METHOD_BOOTSTRAP,
        0U,
        0U,
    };
    static const device_link_channel_t channels[] =
    {
        DEVICE_LINK_CHANNEL_SESSION,
        DEVICE_LINK_CHANNEL_SESSION,
        DEVICE_LINK_CHANNEL_SESSION,
        DEVICE_LINK_CHANNEL_SESSION,
        DEVICE_LINK_CHANNEL_SESSION,
        DEVICE_LINK_CHANNEL_CONTROL,
        DEVICE_LINK_CHANNEL_CONTROL,
    };
    static const uint16_t request_limits[] = {0U, 0U, 80U, 64U, 20U, 32U, 32U};
    static const uint16_t response_limits[] =
    {1005U, 1005U, 256U, 256U, 256U, 3072U, 3072U};

    for (size_t i = 0U; i < 7U; ++i)
    {
        core->methods[i].method_id = (uint8_t)(i + 1U);
        core->methods[i].flags = flags[i];
        core->methods[i].channel = channels[i];
        core->methods[i].permission_id = permissions[i];
        core->methods[i].maximum_request_bytes = request_limits[i];
        core->methods[i].maximum_response_bytes = response_limits[i];
        core->methods[i].request_schema = s_request_schemas[i];
        core->methods[i].response_schema = s_response_schemas[i];
        core->methods[i].response_body_status_mask =
            DEVICE_LINK_STATUS_MASK(DEVICE_LINK_STATUS_OK);
        if (i == 3U)
        {
            core->methods[i].response_body_status_mask |=
                DEVICE_LINK_STATUS_MASK(
                    DEVICE_LINK_STATUS_CONFIRMATION_REQUIRED);
        }
        core->methods[i].handler = _method;
        core->methods[i].handler_arg = core;
    }
    core->domain.domain_id = DEVICE_LINK_DOMAIN_CORE;
    core->domain.major = DEVICE_LINK_CORE_MAJOR;
    core->domain.minor = DEVICE_LINK_CORE_MINOR;
    core->domain.methods = core->methods;
    core->domain.method_count = 7U;
    return device_link_domain_descriptors_validate(&core->domain, 1U);
}
