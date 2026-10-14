#pragma once

// ---- WiFi ayarlari ----
// Bos birakirsan cihaz WiFi olmadan calisir (saat ve web paneli devre disi kalir).
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// Tarayicidan http://xiao-arcade.local adresiyle erisilir
#define DEVICE_HOSTNAME "xiao-arcade"

// Turkiye saati (UTC+3, yaz saati yok)
#define TZ_INFO "<+03>-3"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"

// ---- Foto karuseli ----
// Kaynak JPEG'ler BASELINE olmali (TJpg_Decoder progressive acamaz).
// wsrv.nl proxy'si herhangi bir resmi 240x240 baseline JPEG'e cevirir:
//   https://wsrv.nl/?url=<resim-adresi>&w=240&h=240&fit=cover&output=jpg&q=80
// En fazla 16 foto (her biri PSRAM'de ~115 KB).
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
