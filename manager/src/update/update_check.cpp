#include "update_check.h"
#include "../../sdk/include/eam/version.h"
#include "../../third_party/nlohmann/json.hpp"
#include <charconv>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace eam::updates {

Result ParseRelease(const std::string& text, unsigned currentBuild) {
    const auto release = nlohmann::json::parse(text, nullptr, false);
    if (!release.is_object()) return {Status::Failed};
    const auto tag = release.find("tag_name");
    const auto name = release.find("name");
    if (tag == release.end() || !tag->is_string() || tag->get_ref<const std::string&>().empty() ||
        name == release.end() || !(name->is_string() || name->is_null()) ||
        !release.contains("draft") || !release["draft"].is_boolean() || release["draft"].get<bool>() ||
        !release.contains("prerelease") || !release["prerelease"].is_boolean() || release["prerelease"].get<bool>())
        return {Status::Failed};

    Result result{Status::Available};
    if (!name->is_string()) return result;
    std::string_view title = name->get_ref<const std::string&>();
    const auto first = title.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return result;
    title = title.substr(first, title.find_last_not_of(" \t\r\n") - first + 1);
    constexpr std::string_view prefix = "Build ";
    if (title.substr(0, prefix.size()) != prefix) return result;
    title.remove_prefix(prefix.size());
    unsigned build = 0;
    const auto parsed = std::from_chars(title.data(), title.data() + title.size(), build);
    if (parsed.ec != std::errc{} || parsed.ptr != title.data() + title.size() || build == 0) return result;
    result.build = build;
    result.status = build > currentBuild ? Status::Available : Status::Current;
    return result;
}

std::string BannerText(const Result& result) {
    if (result.status != Status::Available) return {};
    return "Update available" + (result.build ? " (Build " + std::to_string(*result.build) + ")" : std::string());
}

struct Checker::Job {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    Result result{Status::Checking};
};

Checker::Checker(Fetch fetch) : m_fetch(std::move(fetch)) {}
Checker::~Checker() { if (m_job) m_job->cancelled = true; }

void Checker::SetPolicy(Policy policy) {
    if (policy == m_policy) return;
    if (m_job) m_job->cancelled = true;
    m_job.reset();
    m_policy = policy;
    if (policy != Policy::Enabled) return;
    m_job = std::make_shared<Job>();
    try {
        std::thread([job = m_job, fetch = m_fetch] {
            Result result{Status::Failed};
            try {
                if (!job->cancelled) result = ParseRelease(fetch(job->cancelled), EAM_BUILD_NUMBER);
            } catch (...) { /* Network or parse failure is silent and retried next startup. */ }
            std::lock_guard<std::mutex> lock(job->mutex);
            if (!job->cancelled) job->result = result;
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_job->mutex);
        m_job->result = {Status::Failed};
    }
}

Result Checker::Get() const {
    if (!m_job) return {};
    std::lock_guard<std::mutex> lock(m_job->mutex);
    return m_job->result;
}

Checker& Instance() {
    // As with the GUI shell, avoid thread cleanup under DllMain's loader lock.
    static auto* checker = new Checker;
    return *checker;
}

} // namespace eam::updates
