#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace eam::updates {

inline constexpr wchar_t kReleasesUrl[] = L"https://github.com/FriedSnowman/ls-addon-manager/releases/latest";
enum class Policy { Ask, Disabled, Enabled };
enum class Status { Idle, Checking, Current, Available, Failed };
struct Result {
    Status status = Status::Idle;
    std::optional<unsigned> build;
};

// A valid release with an unfamiliar name is available without a build label.
// Error responses and malformed data must never become update notifications.
Result ParseRelease(const std::string& json, unsigned currentBuild);
std::string FetchRelease(const std::atomic<bool>& cancelled);
std::string BannerText(const Result& result);

// Called from the GUI thread. Only the fetch runs in the background; its result
// is shared safely and discarded when the policy changes. No retry loop.
class Checker {
public:
    using Fetch = std::function<std::string(const std::atomic<bool>&)>;
    explicit Checker(Fetch fetch = FetchRelease);
    ~Checker();
    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;
    void SetPolicy(Policy policy);
    Policy GetPolicy() const { return m_policy; }
    Result Get() const;

private:
    struct Job;
    Policy m_policy = Policy::Ask;
    Fetch m_fetch;
    std::shared_ptr<Job> m_job;
};

Checker& Instance();

} // namespace eam::updates
