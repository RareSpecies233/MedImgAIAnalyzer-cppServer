#pragma once

#include <string>
#if defined(__APPLE__)
#include <uuid/uuid.h>
#endif

inline std::string generate_uuid_v4()
{
#if defined(__APPLE__)
    uuid_t id;
    uuid_generate_random(id);
    char buf[37];
    uuid_unparse_lower(id, buf);
    return std::string(buf);
#else
    // 回退实现：简单伪 UUID（非 RFC 完整）——仅供示例使用。
    static const char *chars = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { s.push_back('-'); continue; }
        s.push_back(chars[rand() % 16]);
    }
    return s;
#endif
}
