#include "music_profile.h"

#include <stdio.h>

static void _profile_string(const cJSON *object, const char *key,
                            char *output, size_t output_size)
{
    output[0] = '\0';
    if (object == NULL)
    {
        return;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        (void)snprintf(output, output_size, "%s", item->valuestring);
    }
}

static void _profile_first_string(const cJSON *object,
                                  const char *const *keys, size_t key_count,
                                  char *output, size_t output_size)
{
    output[0] = '\0';
    for (size_t index = 0U; index < key_count; ++index)
    {
        _profile_string(object, keys[index], output, output_size);
        if (output[0] != '\0')
        {
            return;
        }
    }
}

bool music_profile_parse(const cJSON *data, const char *uin, bool qq_login,
                         char *name, size_t name_size,
                         char *avatar, size_t avatar_size)
{
    if (data == NULL || uin == NULL || name == NULL || name_size == 0U ||
            avatar == NULL || avatar_size == 0U)
    {
        return false;
    }
    name[0] = '\0';
    avatar[0] = '\0';
    const cJSON *profile = data;
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(data, "map_userinfo");
    if (cJSON_IsObject(map))
    {
        const cJSON *entry = cJSON_GetObjectItemCaseSensitive(map, uin);
        if (cJSON_IsObject(entry))
        {
            profile = entry;
        }
    }
    if (profile == data)
    {
        const cJSON *user_info =
            cJSON_GetObjectItemCaseSensitive(data, "userInfo");
        if (!cJSON_IsObject(user_info))
        {
            user_info = cJSON_GetObjectItemCaseSensitive(data, "info");
        }
        if (cJSON_IsObject(user_info))
        {
            profile = user_info;
        }
    }

    static const char *const name_keys[] =
    {
        "nick", "nickname", "name", "NickName",
    };
    static const char *const avatar_keys[] =
    {
        "headurl", "headUrl", "avatar", "avatarUrl", "Avatar", "logo",
    };
    _profile_first_string(profile, name_keys,
                          sizeof(name_keys) / sizeof(name_keys[0]),
                          name, name_size);
    _profile_first_string(profile, avatar_keys,
                          sizeof(avatar_keys) / sizeof(avatar_keys[0]),
                          avatar, avatar_size);
    if (avatar[0] == '\0' && qq_login && uin[0] != '\0')
    {
        (void)snprintf(avatar, avatar_size,
                       "https://q.qlogo.cn/headimg_dl?dst_uin=%s&spec=140",
                       uin);
    }
    return name[0] != '\0' || avatar[0] != '\0';
}
