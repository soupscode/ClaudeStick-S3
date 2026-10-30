// Optional: copy to secrets.h to bake defaults into the firmware.
// Anything set over USB serial (wifi / token commands) overrides these.
#pragma once
#define CS_WIFI_SSID ""
#define CS_WIFI_PASS ""
#define CS_TOKEN     ""   // leave empty to auto-generate on first boot
