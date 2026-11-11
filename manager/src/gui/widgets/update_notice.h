#pragma once
#include "../../update/update_check.h"

namespace eam::widgets {

void InitializeUpdateCheck(updates::Checker& checker);
void RenderUpdateConsent(updates::Checker& checker);
void RenderUpdateSettings(updates::Checker& checker);
float UpdateBannerHeight(const updates::Result& result);
float DrawnUpdateBannerHeight();
void RenderUpdateBanner(const updates::Result& result);

} // namespace eam::widgets
