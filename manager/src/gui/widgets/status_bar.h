#pragma once
#include <string>

namespace eam {
namespace widgets {

// One line normally; warnings/errors expand to a two-line reason plus diagnostics.
// Reserve StatusBarHeight(info.level) using the same snapshot passed to StatusBar.
struct StatusBarInfo {
    std::string reason, stats, hardware, details;
    int level = 0;
};
void StatusBar(const StatusBarInfo& info);
void StatusBar(const std::string& left);
float StatusBarHeight(int level = 0);
float DrawnStatusBarHeight(); // for positioning toasts above this frame's footer

} // namespace widgets
} // namespace eam
