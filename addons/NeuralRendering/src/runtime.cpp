// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.h"
#include <windows.h>

#include <array>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace neural {
RuntimeIdentity InspectRuntime(const std::wstring &path) {
    RuntimeIdentity result;
    result.path = std::filesystem::absolute(path).wstring();
    std::ifstream file(std::filesystem::path(result.path), std::ios::binary);
    if (!file)
        throw std::runtime_error("User-provided nvngx_dlssnr.dll was not found");
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot initialize runtime hash");
    std::vector<char> buffer(256 * 1024);
    std::array<unsigned char, 32> digest{};
    bool ok = true;
    while (file) {
        file.read(buffer.data(), buffer.size());
        const auto count = file.gcount();
        if (count && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(count), 0) < 0) {
            ok = false;
            break;
        }
    }
    ok = ok && !file.bad() && BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) >= 0;
    BCryptDestroyHash(hash);
    if (!ok)
        throw std::runtime_error("Cannot hash model runtime");
    constexpr char hex[] = "0123456789abcdef";
    for (auto b : digest) {
        result.sha256 += hex[b >> 4];
        result.sha256 += hex[b & 15];
    }
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(result.path.c_str(), &ignored);
    std::vector<unsigned char> info(bytes);
    if (bytes && GetFileVersionInfoW(result.path.c_str(), 0, bytes, info.data())) {
        VS_FIXEDFILEINFO *fixed = nullptr;
        UINT size = 0;
        if (VerQueryValueW(info.data(), L"\\", reinterpret_cast<void **>(&fixed), &size) && size >= sizeof(*fixed))
            result.version = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." +
                             std::to_string(LOWORD(fixed->dwFileVersionMS)) + "." +
                             std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." +
                             std::to_string(LOWORD(fixed->dwFileVersionLS));
    }
    return result;
}
bool SupportedRuntime(const RuntimeIdentity &identity) {
    // This exact build has passed constant-depth creation/evaluation/retirement tests.
    return identity.sha256 == "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e";
}
} // namespace neural
