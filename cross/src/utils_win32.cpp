#include "utils_win32.h"

#include <windows.h>

namespace platform
{
std::wstring utf8_to_utf16(const std::string_view &str)
{
    if (str.empty())
    {
        return {};
    }

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), static_cast<int>(result.size()));
    return result;
}

std::string utf16_to_utf8(const std::wstring_view &wstr)
{
    if (wstr.empty())
    {
        return {};
    }

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), static_cast<int>(result.size()), nullptr, nullptr);
    return result;
}

bool is_handle_valid(HANDLE handle)
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}
}
