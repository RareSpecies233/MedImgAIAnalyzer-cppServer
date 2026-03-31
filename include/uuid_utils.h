#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#if defined(__APPLE__)
#include <uuid/uuid.h>
#endif

namespace uuid_utils_detail {

inline std::string format_uuid_v4_from_bytes(std::array<unsigned char, 16> bytes)
{
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(bytes[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            oss << '-';
        }
    }
    return oss.str();
}

inline std::string generate_uuid_v4_fallback()
{
    static std::atomic<unsigned long long> counter{0};

    std::array<unsigned char, 16> bytes{};
    std::random_device rd;
    for (auto &byte : bytes) {
        byte = static_cast<unsigned char>(rd() & 0xFFu);
    }

    const auto now = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed) + 1ULL;
    const auto mixed = now ^ (seq * 0x9E3779B97F4A7C15ULL);

    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<unsigned char>(bytes[i] ^ ((mixed >> (i * 8)) & 0xFFu));
    }
    for (size_t i = 0; i < 8; ++i) {
        bytes[8 + i] = static_cast<unsigned char>(bytes[8 + i] ^ ((seq >> (i * 8)) & 0xFFu));
    }

    return format_uuid_v4_from_bytes(bytes);
}

} // namespace uuid_utils_detail

inline std::string generate_uuid_v4()
{
#if defined(__APPLE__)
    uuid_t id;
    uuid_generate_random(id);
    char buf[37];
    uuid_unparse_lower(id, buf);
    return std::string(buf);
#else
    return uuid_utils_detail::generate_uuid_v4_fallback();
#endif
}
