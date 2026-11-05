// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
namespace neural {
struct RuntimeIdentity {
    std::wstring path;
    std::string sha256, version;
};
RuntimeIdentity InspectRuntime(const std::wstring &path);
bool SupportedRuntime(const RuntimeIdentity &);
} // namespace neural
