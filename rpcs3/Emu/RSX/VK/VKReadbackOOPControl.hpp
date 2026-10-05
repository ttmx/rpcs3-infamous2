#pragma once
#include <cstdlib>
#include <cstring>
namespace vk
{
inline bool readback_oop_enabled()
{
    static const bool enabled = []
    {
        const auto* flag = std::getenv("RPCS3_VK_READBACK_OOP");
        return flag && std::strcmp(flag, "1") == 0;
    }();
    return enabled;
}
}
