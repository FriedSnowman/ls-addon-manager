#pragma once
#include <string>

namespace eam {
// Content identity for runtime library filenames and shipped-version labels. Empty if the file cannot be read.
std::string FileSHA256(const std::wstring& path);
}
