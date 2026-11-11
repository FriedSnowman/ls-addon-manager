// Offline by default. --live explicitly smoke-tests the public GitHub API.
#include "src/update/update_check.h"
#include "sdk/include/eam/version.h"
#include "third_party/nlohmann/json.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace eam::updates;
using namespace std::chrono_literals;
static int failed = 0;
static void Check(const char* label, bool ok) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failed;
}
static std::string Release(const std::string& name) {
    return nlohmann::json{{"name", name}, {"tag_name", "test-release"}, {"draft", false}, {"prerelease", false}}.dump();
}
static Result Wait(Checker& checker) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (checker.Get().status == Status::Checking && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
    return checker.Get();
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--live") {
        try {
            std::atomic<bool> cancelled{false};
            const auto result = ParseRelease(FetchRelease(cancelled), EAM_BUILD_NUMBER);
            Check("public GitHub latest release is readable", result.status == Status::Current || result.status == Status::Available);
            if (result.build) std::printf("Latest release: Build %u; installed: Build %u\n", *result.build, EAM_BUILD_NUMBER);
        } catch (const std::exception& e) { Check(e.what(), false); }
        return failed ? 1 : 0;
    }

    const auto newer = ParseRelease(Release("Build 99"), 5);
    Check("newer build includes its number", newer.status == Status::Available && newer.build == 99);
    Check("equal build is current", ParseRelease(Release("Build 5"), 5).status == Status::Current);
    Check("older build is not offered", ParseRelease(Release("Build 4"), 5).status == Status::Current);
    Check("comparison is numeric", ParseRelease(Release("Build 10"), 9).status == Status::Available);
    Check("surrounding whitespace is accepted", ParseRelease(Release(" \nBuild 99\t"), 5).build == 99);
    for (const char* title : {"v2.0.0", "Build", "Build 99beta", "Build -1", "Build 0", "Build 99999999999999999999", ""}) {
        const auto unknown = ParseRelease(Release(title), 5);
        Check("unfamiliar names use the generic banner", unknown.status == Status::Available && !unknown.build && BannerText(unknown) == "Update available");
    }
    Check("build banner text", BannerText(newer) == "Update available (Build 99)");
    Check("no banner for current or failed checks", BannerText({Status::Current, 5}).empty() && BannerText({Status::Failed}).empty());
    for (const char* bad : {"", "not JSON", "<html>Bad gateway</html>", "null", "[]", "{}", "{\"message\":\"API rate limit exceeded\"}", "{\"name\":\"Build 99\"}"})
        Check("invalid/API error response has no update", ParseRelease(bad, 5).status == Status::Failed);
    auto release = nlohmann::json::parse(Release("Build 99"));
    release["name"] = nullptr;
    Check("unnamed valid release uses generic banner", BannerText(ParseRelease(release.dump(), 5)) == "Update available");
    release["name"] = 99;
    Check("wrong name type fails closed", ParseRelease(release.dump(), 5).status == Status::Failed);
    release = nlohmann::json::parse(Release("Build 99"));
    release["draft"] = true;
    Check("draft not offered", ParseRelease(release.dump(), 5).status == Status::Failed);
    release["draft"] = false;
    release["prerelease"] = true;
    Check("prerelease not offered", ParseRelease(release.dump(), 5).status == Status::Failed);

    std::atomic<int> requests{0};
    Checker checker([&](const std::atomic<bool>&) { ++requests; return Release("Build 99"); });
    Check("unconfigured policy is offline and needs consent", checker.GetPolicy() == Policy::Ask && checker.Get().status == Status::Idle && requests == 0);
    checker.SetPolicy(Policy::Disabled);
    Check("declining stays offline", requests == 0 && checker.Get().status == Status::Idle);
    checker.SetPolicy(Policy::Enabled);
    Check("enabling checks immediately", Wait(checker).status == Status::Available && requests == 1);
    checker.SetPolicy(Policy::Enabled);
    Check("unchanged policy does not poll again", requests == 1);
    checker.SetPolicy(Policy::Disabled);
    Check("disabling clears the banner", checker.Get().status == Status::Idle);
    checker.SetPolicy(Policy::Enabled);
    Check("re-enabling starts a fresh check", Wait(checker).status == Status::Available && requests == 2);
    Checker nextStartup([&](const std::atomic<bool>&) { ++requests; return Release("Build 99"); });
    nextStartup.SetPolicy(Policy::Enabled);
    Check("saved opt-in checks on the next startup", Wait(nextStartup).status == Status::Available && requests == 3);
    Checker broken([](const std::atomic<bool>&) -> std::string { throw std::runtime_error("offline"); });
    broken.SetPolicy(Policy::Enabled);
    Check("network failure is contained", Wait(broken).status == Status::Failed);

    // Hold the response until after opting out, then prove it cannot restore a banner.
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, cancelled = false, returned = false;
    Checker slow([&](const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; cv.notify_all();
        cv.wait_for(lock, 3s, [&] { return stop.load(); });
        cancelled = stop;
        returned = true; cv.notify_all();
        return Release("Build 99");
    });
    slow.SetPolicy(Policy::Enabled);
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check("startup returns while fetch is pending", cv.wait_for(lock, 3s, [&] { return entered; }) && slow.Get().status == Status::Checking);
    }
    slow.SetPolicy(Policy::Disabled);
    cv.notify_all();
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check("opt-out cancels pending work", cv.wait_for(lock, 3s, [&] { return returned; }) && cancelled);
    }
    Check("late response cannot restore the banner", slow.Get().status == Status::Idle);
    std::printf("\nUPDATE TEST %s (%d failed)\n", failed ? "FAILED" : "PASSED", failed);
    return failed ? 1 : 0;
}
