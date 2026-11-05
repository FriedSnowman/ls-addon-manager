#pragma once
// The runtime files the addons use, as a short fixed list at the bottom of the addon list: one line each, a tick when the file is loaded
// (the addon runs on it now), a circle when its addon is on and waits for a game to be scaled, a cross when its addon is off;
// the runtime name, version and availability; + to choose another file. Hovering a line shows its description, path and which addon uses it.
#include "../../addon/runtime_files.h"
#include <vector>

namespace eam {
namespace widgets {

struct RuntimeAction { int index = -1; enum Kind { None, Choose } kind = None; };

// The list at the bottom of the window it is drawn in (right under what came before when there is no room below), a separator above it.
// openMenu: a line whose + menu is shown open (the preview's pictures); -1 none.
RuntimeAction RuntimeListAtBottom(const std::vector<RuntimeFile>& rows, int openMenu = -1);

} // namespace widgets
} // namespace eam
