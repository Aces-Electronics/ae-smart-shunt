#pragma once

// OTA server settings
#define OTA_SERVER "api.github.com"
#define OTA_PORT 443

// Helpers to stringify macro values safely
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

// Expect these macros to be provided at build time:
//   -DOTAGH_OWNER_NAME=myuser
//   -DOTAGH_REPO_NAME=myrepo
// Optional:
//   -DOTAGH_BEARER=your_token

// Construct GitHub API paths
#define OTA_CHECK_PATH "/repos/" STR(OTAGH_OWNER_NAME) "/" STR(OTAGH_REPO_NAME) "/releases/latest"
#define OTA_BIN_PATH   "/repos/" STR(OTAGH_OWNER_NAME) "/" STR(OTAGH_REPO_NAME) "/releases/assets/"

#ifdef OTAGH_BEARER
#define OTA_BEARER STR(OTAGH_BEARER)
#endif

#define FIRMWARE_BIN_MATCH "firmware.bin"

#include <Arduino.h>

// Helper to construct a full asset endpoint from asset_id
inline String OTA_ASSET_ENDPOINT_CONSTRUCTOR(const String &asset_id)
{
    return String(OTA_BIN_PATH) + asset_id;
}
