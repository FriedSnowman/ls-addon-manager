#pragma once

// The name the user sees (tray, window title, status bar, About, file names). Change it here and everywhere follows.
#define EAM_PRODUCT_NAME "Addon Manager for Lossless Scaling"
#define EAM_PRODUCT_NAME_W L"Addon Manager for Lossless Scaling"
#define EAM_PRODUCT_SHORT "LS Addon Manager"          // where space is tight (the status bar, the tray) and in running text
#define EAM_PRODUCT_SHORT_W L"LS Addon Manager"
// Called Echo Addon Manager up to 0.8.0; the author stays Echo-Storm.
#define EAM_PRODUCT_FILE "LSAddonManager"   // used in file names (log, backups, diagnostics)
#define EAM_PRODUCT_LOGFILE_W L"LSAddonManager.log"

// Increment once per release: Build 1, Build 2, ... (not on every compilation).
// Windows file resources store the corresponding numeric version as 1.0.<build>.0.
#define EAM_BUILD_NUMBER 1
#define EAM_STRINGIFY_IMPL(value) #value
#define EAM_STRINGIFY(value) EAM_STRINGIFY_IMPL(value)
#define EAM_VERSION_STRING "Build " EAM_STRINGIFY(EAM_BUILD_NUMBER)

// The addon API: what an addon can rely on. This is what IHost::GetHostVersion() reports and what an addon's
// "min_host_version" is compared with (0x00MMmmpp). Bump the minor number when calls are added to IHost (always
// appended at the end, so older addons keep working); bump the major number only for a break. It moves on its own,
// not with the release number above.
#define EAM_API_VERSION_MAJOR 1
#define EAM_API_VERSION_MINOR 4
#define EAM_API_VERSION_PATCH 0
#define EAM_API_VERSION_STRING "1.4.0"
#define EAM_API_VERSION_INT ((EAM_API_VERSION_MAJOR << 16) | (EAM_API_VERSION_MINOR << 8) | EAM_API_VERSION_PATCH)
