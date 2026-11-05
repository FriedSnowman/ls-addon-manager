#pragma once
#include <string>

namespace eam {
namespace widgets {

// A slim bar along the bottom of the window with status text in a muted colour. Call it at
// the end of the window, after the content child, and reserve StatusBarHeight() for it.
void StatusBar(const std::string& left);
float StatusBarHeight();

} // namespace widgets
} // namespace eam
