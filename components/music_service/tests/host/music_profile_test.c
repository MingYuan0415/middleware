#include "music_profile.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static cJSON *_parse(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    return root;
}

static void test_map_userinfo(void)
{
    cJSON *root = _parse(
                      "{\"map_userinfo\":{\"123456789\":{"
                      "\"nick\":\"昵称一\","
                      "\"headurl\":\"https://avatar.example/one.png\"}}}");
    char name[64];
    char avatar[512];
    assert(music_profile_parse(root, "123456789", false,
                               name, sizeof(name), avatar, sizeof(avatar)));
    assert(strcmp(name, "昵称一") == 0);
    assert(strcmp(avatar, "https://avatar.example/one.png") == 0);
    cJSON_Delete(root);
}

static void test_userinfo_aliases(void)
{
    cJSON *root = _parse(
                      "{\"userInfo\":{\"nickname\":\"昵称二\","
                      "\"avatarUrl\":\"https://avatar.example/two.png\"}}");
    char name[64];
    char avatar[512];
    assert(music_profile_parse(root, "123456789", false,
                               name, sizeof(name), avatar, sizeof(avatar)));
    assert(strcmp(name, "昵称二") == 0);
    assert(strcmp(avatar, "https://avatar.example/two.png") == 0);
    cJSON_Delete(root);

    root = _parse(
               "{\"info\":{\"NickName\":\"昵称三\","
               "\"Avatar\":\"https://avatar.example/three.png\"}}");
    assert(music_profile_parse(root, "123456789", false,
                               name, sizeof(name), avatar, sizeof(avatar)));
    assert(strcmp(name, "昵称三") == 0);
    assert(strcmp(avatar, "https://avatar.example/three.png") == 0);
    cJSON_Delete(root);
}

static void test_map_miss_falls_back_to_userinfo(void)
{
    cJSON *root = _parse(
                      "{\"map_userinfo\":{\"other\":{\"nick\":\"错误\"}},"
                      "\"userInfo\":{\"nick\":\"正确\"}}");
    char name[64];
    char avatar[512];
    assert(music_profile_parse(root, "123456789", false,
                               name, sizeof(name), avatar, sizeof(avatar)));
    assert(strcmp(name, "正确") == 0);
    assert(avatar[0] == '\0');
    cJSON_Delete(root);
}

static void test_qq_avatar_fallback(void)
{
    cJSON *root = _parse("{\"map_userinfo\":{\"123456789\":{}}}");
    char name[64];
    char avatar[512];
    assert(music_profile_parse(root, "123456789", true,
                               name, sizeof(name), avatar, sizeof(avatar)));
    assert(name[0] == '\0');
    assert(strcmp(avatar,
                  "https://q.qlogo.cn/headimg_dl?dst_uin=123456789&spec=140")
           == 0);
    assert(!music_profile_parse(root, "123456789", false,
                                name, sizeof(name), avatar, sizeof(avatar)));
    assert(name[0] == '\0');
    assert(avatar[0] == '\0');
    cJSON_Delete(root);
}

static void test_invalid_inputs(void)
{
    char name[64];
    char avatar[512];
    assert(!music_profile_parse(NULL, "123456789", false,
                                name, sizeof(name), avatar, sizeof(avatar)));
    cJSON *root = _parse("{}");
    assert(!music_profile_parse(root, NULL, false,
                                name, sizeof(name), avatar, sizeof(avatar)));
    assert(!music_profile_parse(root, "123456789", false,
                                NULL, sizeof(name), avatar, sizeof(avatar)));
    assert(!music_profile_parse(root, "123456789", false,
                                name, 0U, avatar, sizeof(avatar)));
    cJSON_Delete(root);
}

int main(void)
{
    test_map_userinfo();
    test_userinfo_aliases();
    test_map_miss_falls_back_to_userinfo();
    test_qq_avatar_fallback();
    test_invalid_inputs();
    printf("music_profile_test: PASS\n");
    return 0;
}
