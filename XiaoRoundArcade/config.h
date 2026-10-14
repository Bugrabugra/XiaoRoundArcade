#pragma once

// ---- WiFi settings ----
// Leave empty to run without WiFi (clock and web panel are disabled).
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// Reachable from a browser at http://xiao-arcade.local
#define DEVICE_HOSTNAME "xiao-arcade"

// Turkey time (UTC+3, no DST)
#define TZ_INFO "<+03>-3"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"

// ---- Photo carousel ----
// Source JPEGs must be BASELINE (TJpg_Decoder can't decode progressive).
// The wsrv.nl proxy converts any image to a 240x240 baseline JPEG:
//   https://wsrv.nl/?url=<image-url>&w=240&h=240&fit=cover&output=jpg&q=80
// Up to 16 photos (~115 KB of PSRAM each).
#define WSRV_PICSUM(id) "https://wsrv.nl/?url=picsum.photos/id/" #id "/600/600&w=240&h=240&fit=cover&output=jpg&q=80"
static const char *const PHOTO_URLS[] = {
  WSRV_PICSUM(1015),
  WSRV_PICSUM(1018),
  WSRV_PICSUM(1039),
  WSRV_PICSUM(1043),
  WSRV_PICSUM(1069),
  WSRV_PICSUM(1074),
  WSRV_PICSUM(237),
  WSRV_PICSUM(1025),
};
