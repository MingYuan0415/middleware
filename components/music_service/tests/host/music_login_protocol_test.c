#include "music_login_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_hash33_matches_qrsig_token(void)
{
    const char *qrsig =
        "8716fefe7142ff98793c19ed9d28e8351ab706236b9af1a310baf1c9f8a992dd"
        "e6b03f5bedab76d337d38f54e737dd00fc490c33568b621a7ca3f10a80fde63d";
    assert(music_login_hash33(qrsig, 0U) == 999655772U);
    assert(music_login_hash33(NULL, 5381U) == 5381U);
}

static void test_ptui_cb_states(void)
{
    char musicid[32];
    char url[256];
    assert(music_login_parse_ptui_cb(
               "ptuiCB('66','0','','0','二维码未失效。','')",
               musicid, sizeof(musicid), url, sizeof(url)) ==
           MUSIC_LOGIN_QQ_WAITING);
    assert(music_login_parse_ptui_cb(
               "ptuiCB('67','0','','0','二维码认证中。','')",
               musicid, sizeof(musicid), url, sizeof(url)) ==
           MUSIC_LOGIN_QQ_SCANNED);
    assert(music_login_parse_ptui_cb(
               "ptuiCB('65','0','','0','二维码已失效。','')",
               musicid, sizeof(musicid), url, sizeof(url)) ==
           MUSIC_LOGIN_QQ_FAILED);
    assert(music_login_parse_ptui_cb(
               "ptuiCB('3','0','','0','本次登录已被拒绝。','')",
               musicid, sizeof(musicid), url, sizeof(url)) ==
           MUSIC_LOGIN_QQ_FAILED);
}

static void test_ptui_cb_success_fields(void)
{
    char musicid[32];
    char url[256];
    const char *body =
        "ptuiCB('0','0','https://ptlogin2.qq.com/check_sig?pttype=1"
        "&uin=123456789&service=QQ&ptsigx=abc','0','登录成功','')";
    assert(music_login_parse_ptui_cb(body, musicid, sizeof(musicid),
                                     url, sizeof(url)) ==
           MUSIC_LOGIN_QQ_SUCCESS);
    assert(strcmp(musicid, "123456789") == 0);
    assert(strcmp(url,
                  "https://ptlogin2.qq.com/check_sig?pttype=1"
                  "&uin=123456789&service=QQ&ptsigx=abc") == 0);
}

static void test_cookie_parse(void)
{
    char name[32];
    char value[64];
    assert(music_login_cookie_parse(
               "qrsig=abc123; Path=/; Domain=ptlogin2.qq.com",
               name, sizeof(name), value, sizeof(value)));
    assert(strcmp(name, "qrsig") == 0);
    assert(strcmp(value, "abc123") == 0);
    assert(!music_login_cookie_parse("; Path=/", name, sizeof(name),
                                     value, sizeof(value)));
}

static void test_png_and_query_helpers(void)
{
    static const uint8_t png[] =
    {
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U
    };
    assert(music_login_png_signature(png, sizeof(png)));
    assert(!music_login_png_signature((const uint8_t *)"GIF89a", 6U));

    char code[32];
    assert(music_login_query_value(
               "https://y.qq.com/portal/wx_redirect.html?login_type=1"
               "&code=ABC%2F123&state=state",
               "code", code, sizeof(code)));
    assert(strcmp(code, "ABC/123") == 0);
    assert(!music_login_query_value(
               "https://y.qq.com/?no_code=1", "code", code, sizeof(code)));
}

static void test_form_encoding(void)
{
    char body[512];
    size_t used = 0U;
    body[0] = '\0';
    assert(music_login_form_add(body, sizeof(body), &used,
                                "redirect_uri",
                                "https://y.qq.com/portal/wx_redirect.html"
                                "?login_type=1&surl=https://y.qq.com/#"));
    assert(strstr(body,
                  "redirect_uri=https%3A%2F%2Fy.qq.com%2Fportal%2F"
                  "wx_redirect.html%3Flogin_type%3D1%26surl%3Dhttps%3A%2F%2F"
                  "y.qq.com%2F%23") != NULL);
    assert(music_login_form_add(body, sizeof(body), &used, "state", "state"));
    assert(strstr(body, "&state=state") != NULL);
}

int main(void)
{
    test_hash33_matches_qrsig_token();
    test_ptui_cb_states();
    test_ptui_cb_success_fields();
    test_cookie_parse();
    test_png_and_query_helpers();
    test_form_encoding();
    printf("music_login_protocol_test: PASS\n");
    return 0;
}
