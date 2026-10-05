#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
#endif

namespace vk::readback_copy
{
    inline bool enabled()
    {
        static const bool value = []
        {
            const char* option = std::getenv("RPCS3_VK_READBACK_STREAM_COPY");
            return option && std::strcmp(option, "1") == 0;
        }();
        return value;
    }

    inline bool validation_enabled()
    {
        static const bool value = []
        {
            const char* option = std::getenv("RPCS3_VK_READBACK_STREAM_VALIDATE");
            return option && std::strcmp(option, "1") == 0;
        }();
        return value;
    }

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
    // Non-temporal writes eliminate destination write allocation for the measured
    // multi-megabyte cached staging -> protected guest-memory retirement. The
    // caller retains all original event waits, DMA/cache locks and VM protection.
    // Source is a distinct, completed normal Vulkan host allocation; imported
    // guest-memory DMA blocks never call this function.
    __attribute__((target("avx512f"), noinline))
    inline void stream(void* destination, const void* source, std::size_t length)
    {
        auto* dst = static_cast<unsigned char*>(destination);
        const auto* src = static_cast<const unsigned char*>(source);
        const std::size_t prefix = (-reinterpret_cast<std::uintptr_t>(dst)) & 63u;
        if (prefix)
        {
            std::memcpy(dst, src, prefix);
            dst += prefix;
            src += prefix;
            length -= prefix;
        }
        while (length >= 256)
        {
            const __m512i a = _mm512_loadu_si512(src);
            const __m512i b = _mm512_loadu_si512(src + 64);
            const __m512i c = _mm512_loadu_si512(src + 128);
            const __m512i d = _mm512_loadu_si512(src + 192);
            _mm512_stream_si512(reinterpret_cast<__m512i*>(dst), a);
            _mm512_stream_si512(reinterpret_cast<__m512i*>(dst + 64), b);
            _mm512_stream_si512(reinterpret_cast<__m512i*>(dst + 128), c);
            _mm512_stream_si512(reinterpret_cast<__m512i*>(dst + 192), d);
            src += 256;
            dst += 256;
            length -= 256;
        }
        while (length >= 64)
        {
            _mm512_stream_si512(reinterpret_cast<__m512i*>(dst), _mm512_loadu_si512(src));
            src += 64;
            dst += 64;
            length -= 64;
        }
        // Flush all streaming stores before returning to the original cache
        // retirement/unprotect path or a consuming SPU on another host thread.
        _mm_sfence();
        if (length)
            std::memcpy(dst, src, length);
        _mm256_zeroupper();
    }
#endif

    // false means untouched: the original memcpy must execute. Flags are the
    // actual immutable allocation-selected Vulkan memory flags (not preferences).
    inline bool copy(void* destination, const void* source, std::size_t length,
        std::uint32_t allocation_flags, bool avx512_available)
    {
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
        constexpr std::uint32_t host_visible_coherent_cached = 2u | 4u | 8u;
        if (!avx512_available || !destination || !source || length < 256u * 1024u ||
            (allocation_flags & host_visible_coherent_cached) != host_visible_coherent_cached)
            return false;
        const auto dst = reinterpret_cast<std::uintptr_t>(destination);
        const auto src = reinterpret_cast<std::uintptr_t>(source);
        const auto maximum = std::numeric_limits<std::uintptr_t>::max();
        if (length > maximum - dst || length > maximum - src ||
            (dst <= src ? src - dst < length : dst - src < length))
            return false;
        stream(destination, source, length);
        return true;
#else
        (void)destination;
        (void)source;
        (void)length;
        (void)allocation_flags;
        (void)avx512_available;
        return false;
#endif
    }
}
