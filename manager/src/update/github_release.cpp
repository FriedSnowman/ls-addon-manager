#include "update_check.h"
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include <stdexcept>

namespace eam::updates {
namespace {

struct InternetHandle {
    HINTERNET value;
    explicit InternetHandle(HINTERNET handle) : value(handle) {
        if (!value) throw std::runtime_error("Could not open update request");
    }
    ~InternetHandle() { WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

void Require(BOOL ok) {
    if (!ok) throw std::runtime_error("Update request failed");
}

} // namespace

std::string FetchRelease(const std::atomic<bool>& cancelled) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    const auto stopped = [&] { return cancelled || Clock::now() >= deadline; };
    if (stopped()) return {};
    InternetHandle session(WinHttpOpen(L"LSAddonManager-UpdateCheck", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    Require(WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000));
    InternetHandle connection(WinHttpConnect(session.value, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    InternetHandle request(WinHttpOpenRequest(connection.value, L"GET",
        L"/repos/FriedSnowman/ls-addon-manager/releases/latest", nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_REDIRECTS;
    Require(WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof disabled));
    constexpr wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2026-03-10\r\n";
    if (stopped()) return {};
    Require(WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0));
    if (stopped()) return {};
    Require(WinHttpReceiveResponse(request.value, nullptr));
    DWORD status = 0, size = sizeof status;
    Require(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX));
    if (status != 200) return {}; // Includes no release, rate limits and server failures; no retry this startup.
    std::string body;
    constexpr size_t maxBytes = 1024 * 1024;
    while (!stopped()) {
        char buffer[8192];
        DWORD read = 0;
        Require(WinHttpReadData(request.value, buffer, sizeof buffer, &read));
        if (!read) return body;
        if (body.size() + read > maxBytes) return {};
        body.append(buffer, read);
    }
    return {};
}

} // namespace eam::updates
