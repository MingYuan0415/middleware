#ifndef __MUSIC_PROFILE_H__
#define __MUSIC_PROFILE_H__

#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse the account profile fields from one user-info response.
 *
 * @param data Response data object.
 * @param uin Account id used as the map key and QQ avatar fallback.
 * @param qq_login Whether the QQ avatar fallback is allowed.
 * @param name Output nickname buffer.
 * @param name_size Output buffer size.
 * @param avatar Output avatar URL buffer.
 * @param avatar_size Output buffer size.
 * @return true when a nickname or avatar was found.
 */
bool music_profile_parse(const cJSON *data, const char *uin, bool qq_login,
                         char *name, size_t name_size,
                         char *avatar, size_t avatar_size);

#ifdef __cplusplus
}
#endif

#endif /* __MUSIC_PROFILE_H__ */
