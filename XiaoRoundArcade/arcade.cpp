/*
 * XIAO ARCADE  -  Seeed XIAO ESP32S3 (Sense) + Seeed Round Display (dokunmatik)
 * -------------------------------------------------------------------------
 *  Uygulamalar (ana menuden dokunarak secilir):
 *   1) ORBIT GUARD : Gezegeni meteorlardan koru. Ekranin neresine dokunursan
 *                    kalkan o yone doner. Yesil kalpleri ICERI birak (+can),
 *                    altin yildizlari yakala (+puan, genis kalkan).
 *   2) CIZIM       : Dokunmatik testi / parmakla resim. Kenardaki halkadan renk
 *                    sec, ustteki "C" temizler, alttaki "X" menuye doner.
 *   3) FOTO        : Internetten indirilen fotolar, karusel. Saga/sola kaydir,
 *                    dokun = otomatik slayt, asagi cek = menu. (PSRAM + WiFi)
 *   4) SAAT        : WiFi + NTP ile analog/dijital saat. Dokun -> menu.
 *
 *  WiFi baglaninca: http://xiao-arcade.local (veya IP) adresinde web paneli:
 *   - rekor / istatistikler
 *   - telefondan ekrana mesaj gonderme
 *
 *  Kutuphane: Seeed_GFX (TFT_eSPI tabanli). driver.h icinde BOARD_SCREEN_COMBO 501.
 */
#include "driver.h"
#include "config.h"

#include <Arduino.h>
// Not: WebServer, TFT_eSPI'dan ONCE gelmeli (Seeed_GFX FS_NO_GLOBALS tanimliyor)
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <TJpg_Decoder.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>

#ifndef TOUCH_INT
#define TOUCH_INT D7
#endif
#define TOUCH_I2C_ADDR 0x2E

// ---------------------------------------------------------------------------
// Genel
// ---------------------------------------------------------------------------
static const int16_t W = 240, H = 240, CX = 120, CY = 120;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
Preferences prefs;
WebServer server(80);

enum App { APP_MENU, APP_GAME, APP_PAINT, APP_PHOTO, APP_CLOCK };
App app = APP_MENU;

// Renkler (RGB565)
#define C_BG       0x0000
#define C_DIM      0x2945
#define C_RING     0x18E3
#define C_CYAN     0x07FF
#define C_SHIELD   0x5EFF
#define C_GOLD     0xFEA0
#define C_ROCK     0xC408
#define C_FAST     0xF8A3
#define C_HEART    0x37E6
#define C_PLANET   0x2B7F
#define C_LAND     0x2E68

// Istatistikler (NVS'de saklanir)
uint32_t bestScore = 0, gamesPlayed = 0, totalBlocks = 0;

// Web'den gelen mesaj (toast)
char toastText[40] = "";
uint32_t toastUntil = 0;

// WiFi durumu
bool wifiEnabled = false, netStarted = false;

// ---------------------------------------------------------------------------
// Yardimcilar
// ---------------------------------------------------------------------------
static float angDiff(float a, float b) {
  float d = a - b;
  while (d > PI) d -= TWO_PI;
  while (d < -PI) d += TWO_PI;
  return d;
}

// Matematik acisi (radyan, 0 = saat 3, saat yonunde) -> TFT_eSPI arc acisi (0 = saat 6)
static uint32_t toArcDeg(float rad) {
  int d = (int)lroundf(rad * 180.0f / PI) - 90;
  d %= 360;
  if (d < 0) d += 360;
  return (uint32_t)d;
}

static float frand(float a, float b) { return a + (b - a) * (random(10000) / 10000.0f); }

// UTF-8 Turkce karakterleri ASCII'ye cevir (dahili fontlar sadece ASCII)
static void toAsciiTR(const String &in, char *out, size_t outLen) {
  size_t o = 0;
  for (size_t i = 0; i < in.length() && o + 1 < outLen; i++) {
    uint8_t c = in[i];
    if (c < 0x80) { if (c >= 32) out[o++] = c; continue; }
    if (i + 1 >= in.length()) break;
    uint8_t d = in[i + 1];
    char r = 0;
    if (c == 0xC3) {
      if (d == 0xA7) r = 'c'; else if (d == 0x87) r = 'C';
      else if (d == 0xB6) r = 'o'; else if (d == 0x96) r = 'O';
      else if (d == 0xBC) r = 'u'; else if (d == 0x9C) r = 'U';
    } else if (c == 0xC4) {
      if (d == 0x9F) r = 'g'; else if (d == 0x9E) r = 'G';
      else if (d == 0xB1) r = 'i'; else if (d == 0xB0) r = 'I';
    } else if (c == 0xC5) {
      if (d == 0x9F) r = 's'; else if (d == 0x9E) r = 'S';
    }
    if (r) out[o++] = r;
    // cok baytli diger karakterleri atla
    if (c >= 0xC0) { i++; if (c >= 0xE0) i++; if (c >= 0xF0) i++; }
  }
  out[o] = 0;
}

static void saveStats() {
  prefs.putUInt("best", bestScore);
  prefs.putUInt("plays", gamesPlayed);
  prefs.putUInt("blocks", totalBlocks);
}

// ---------------------------------------------------------------------------
// Dokunmatik (CHSC6X, I2C 0x2E, INT = D7 aktif LOW)
// ---------------------------------------------------------------------------
struct TouchState {
  bool down = false, pressed = false, released = false;
  int16_t x = 0, y = 0;
} touch;

static bool physDown = false;
static bool waitForRelease = false;
static uint32_t lastTouchSeen = 0;

static bool readTouchRaw(int16_t &x, int16_t &y) {
  if (digitalRead(TOUCH_INT) != LOW) {
    delayMicroseconds(1000);
    if (digitalRead(TOUCH_INT) != LOW) return false;
  }
  uint8_t buf[5];
  if (Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)5) != 5) return false;
  Wire.readBytes(buf, 5);
  if (buf[0] != 0x01) return false;  // 1 = parmak var
  x = buf[2];
  y = buf[4];
  return true;
}

static void updateTouch() {
  int16_t x, y;
  uint32_t now = millis();
  if (readTouchRaw(x, y)) {
    lastTouchSeen = now;
    touch.x = x;
    touch.y = y;
    physDown = true;
  } else if (physDown && now - lastTouchSeen > 60) {
    physDown = false;  // kisa kesintileri yut (60 ms)
  }

  bool down = physDown;
  if (waitForRelease) {
    if (!physDown) waitForRelease = false;
    down = false;
  }
  touch.pressed = down && !touch.down;
  touch.released = !down && touch.down;
  touch.down = down;
}

static float touchDist() { return hypotf(touch.x - CX, touch.y - CY); }
static float touchAngle() { return atan2f(touch.y - CY, touch.x - CX); }

// ---------------------------------------------------------------------------
// Ortak cizimler
// ---------------------------------------------------------------------------
struct Star { float a, r, spd; uint8_t b; };
Star stars[70];

static void initStars() {
  for (auto &s : stars) {
    s.a = frand(0, TWO_PI);
    s.r = frand(10, 125);
    s.spd = frand(0.02f, 0.12f);
    s.b = random(60, 255);
  }
}

static void drawStars(float dt, float spin = 1.0f) {
  for (auto &s : stars) {
    s.a += s.spd * dt * spin;
    int x = CX + cosf(s.a) * s.r, y = CY + sinf(s.a) * s.r;
    uint8_t b = s.b;
    if (random(100) < 2) b = 255;  // parilti
    spr.drawPixel(x, y, spr.color565(b, b, b));
  }
}

static void drawHeart(int x, int y, int s, uint16_t col) {
  int h = s / 2;
  spr.fillCircle(x - h, y - h / 2, h, col);
  spr.fillCircle(x + h, y - h / 2, h, col);
  spr.fillTriangle(x - s, y - h / 3, x + s, y - h / 3, x, y + s, col);
}

static void drawToast() {
  if (!toastText[0] || millis() > toastUntil) return;
  spr.fillRoundRect(30, 176, 180, 28, 14, 0x1A6F);
  spr.drawRoundRect(30, 176, 180, 28, 14, C_CYAN);
  spr.setTextFont(2);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE);
  spr.drawString(toastText, CX, 190);
}

// ---------------------------------------------------------------------------
// Uygulama gecisleri
// ---------------------------------------------------------------------------
void enterMenu();
void enterGame();
void enterPaint();
void enterClock();
void enterPhoto();

static void switchApp(App a) {
  app = a;
  waitForRelease = true;  // onceki dokunus yeni ekranda tiklama sayilmasin
  switch (a) {
    case APP_MENU:  enterMenu();  break;
    case APP_GAME:  enterGame();  break;
    case APP_PAINT: enterPaint(); break;
    case APP_CLOCK: enterClock(); break;
    case APP_PHOTO: enterPhoto(); break;
  }
}

// ---------------------------------------------------------------------------
// MENU
// ---------------------------------------------------------------------------
struct Icon { int16_t x, y; const char *label; App target; };
const Icon icons[4] = {
  {45, 122, "OYUN", APP_GAME},
  {95, 122, "CIZIM", APP_PAINT},
  {145, 122, "FOTO", APP_PHOTO},
  {195, 122, "SAAT", APP_CLOCK},
};
float menuT = 0;

void enterMenu() { menuT = 0; }

static void drawIconGame(int x, int y, float t) {
  spr.fillCircle(x, y, 7, C_PLANET);
  spr.fillCircle(x - 2, y - 2, 2, C_LAND);
  float a = t * 1.5f;
  spr.drawArc(x, y, 15, 12, toArcDeg(a - 0.6f), toArcDeg(a + 0.6f), C_SHIELD, 0x10A2, true);
  float ma = a + PI;
  spr.fillCircle(x + cosf(ma) * 18, y + sinf(ma) * 18, 2, C_ROCK);
}

static void drawIconPaint(int x, int y, float t) {
  const uint16_t cols[5] = {TFT_RED, TFT_YELLOW, TFT_GREEN, C_CYAN, TFT_MAGENTA};
  for (int i = 0; i < 5; i++) {
    float a = t + i * TWO_PI / 5;
    spr.fillCircle(x + cosf(a) * 12, y + sinf(a) * 12, 4, cols[i]);
  }
  spr.fillCircle(x, y, 3, TFT_WHITE);
}

static void drawIconClock(int x, int y) {
  spr.drawCircle(x, y, 16, TFT_WHITE);
  struct tm tmv;
  float hA = -HALF_PI, mA = -HALF_PI;
  if (getLocalTime(&tmv, 0)) {
    mA = tmv.tm_min / 60.0f * TWO_PI - HALF_PI;
    hA = ((tmv.tm_hour % 12) + tmv.tm_min / 60.0f) / 12.0f * TWO_PI - HALF_PI;
  }
  spr.drawWideLine(x, y, x + cosf(hA) * 8, y + sinf(hA) * 8, 3, TFT_WHITE, 0x10A2);
  spr.drawWideLine(x, y, x + cosf(mA) * 13, y + sinf(mA) * 13, 2, C_CYAN, 0x10A2);
}

static void drawIconPhoto(int x, int y, float t) {
  spr.drawRoundRect(x - 14, y - 11, 28, 22, 4, TFT_WHITE);
  spr.fillCircle(x + 6, y - 4, 3, C_GOLD);
  spr.fillTriangle(x - 12, y + 9, x - 3, y - 3, x + 6, y + 9, C_LAND);
  spr.fillTriangle(x - 2, y + 9, x + 5, y + 1, x + 12, y + 9, 0x2C4A);
}

void loopMenu(float dt) {
  menuT += dt;

  if (touch.pressed) {
    for (auto &ic : icons) {
      if (hypotf(touch.x - ic.x, touch.y - ic.y) < 25) { switchApp(ic.target); return; }
    }
  }

  spr.fillSprite(C_BG);
  drawStars(dt, 2.0f);

  // Kenarda donen uydu
  float sa = menuT * 0.8f;
  spr.drawCircle(CX, CY, 114, C_RING);
  spr.fillSmoothCircle(CX + cosf(sa) * 114, CY + sinf(sa) * 114, 4, C_CYAN, C_BG);

  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(C_CYAN);
  spr.setTextFont(4);
  spr.drawString("XIAO ARCADE", CX, 52);
  spr.setTextFont(2);
  spr.setTextColor(C_GOLD);
  spr.drawString(String("Rekor: ") + bestScore, CX, 78);

  for (auto &ic : icons) {
    bool hot = touch.down && hypotf(touch.x - ic.x, touch.y - ic.y) < 25;
    spr.fillSmoothCircle(ic.x, ic.y, 22, hot ? 0x3186 : 0x10A2, C_BG);
    spr.drawSmoothCircle(ic.x, ic.y, 22, hot ? TFT_WHITE : C_DIM, C_BG);
    if (ic.target == APP_GAME) drawIconGame(ic.x, ic.y, menuT);
    else if (ic.target == APP_PAINT) drawIconPaint(ic.x, ic.y, menuT);
    else if (ic.target == APP_PHOTO) drawIconPhoto(ic.x, ic.y, menuT);
    else drawIconClock(ic.x, ic.y);
    spr.setTextFont(2);
    spr.setTextColor(TFT_WHITE);
    spr.drawString(ic.label, ic.x, ic.y + 34);
  }

  spr.setTextFont(2);
  if (!wifiEnabled) {
    spr.setTextColor(C_DIM);
    spr.drawString("WiFi kapali (config.h)", CX, 190);
  } else if (WiFi.status() == WL_CONNECTED) {
    spr.setTextColor(TFT_GREEN);
    spr.drawString(WiFi.localIP().toString(), CX, 184);
    spr.setTextColor(C_DIM);
    spr.drawString(DEVICE_HOSTNAME ".local", CX, 201);
  } else {
    spr.setTextColor(TFT_ORANGE);
    spr.drawString("WiFi baglaniyor...", CX, 190);
  }

  drawToast();
  spr.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// ORBIT GUARD
// ---------------------------------------------------------------------------
static const float PLANET_R = 16, SHIELD_R = 58, SPAWN_R = 128;
static const float SHIELD_HALF = 0.50f, SHIELD_HALF_WIDE = 0.85f;
static const uint8_t MAX_LIVES = 5;

enum MeteorType : uint8_t { M_ROCK, M_FAST, M_GOLD, M_HEART };
struct Meteor { bool alive; bool passed; float a, r, spd, size; MeteorType type; };
struct Particle { bool alive; float x, y, vx, vy, life; uint16_t col; };
struct Popup { bool alive; float x, y, life; char txt[16]; uint16_t col; };

Meteor meteors[20];
Particle parts[90];
Popup popups[6];

enum GameState { GS_READY, GS_PLAY, GS_OVER };
GameState gs;
float shieldA, shieldHalf;
uint32_t score, combo, level;
uint8_t lives;
float spawnTimer, wideTimer, hitFlash, shakeT, playTime, overTimer;
bool newRecord;

static uint16_t meteorColor(uint8_t t) {
  switch (t) {
    case M_FAST:  return C_FAST;
    case M_GOLD:  return C_GOLD;
    case M_HEART: return C_HEART;
    default:      return C_ROCK;
  }
}

static void burst(float x, float y, uint16_t col, int n, float spd) {
  for (auto &p : parts) {
    if (n <= 0) break;
    if (p.alive) continue;
    float a = frand(0, TWO_PI), v = frand(spd * 0.3f, spd);
    p = {true, x, y, cosf(a) * v, sinf(a) * v, frand(0.3f, 0.8f), col};
    n--;
  }
}

static void popup(float x, float y, const char *txt, uint16_t col) {
  for (auto &p : popups) {
    if (p.alive) continue;
    p.alive = true; p.x = x; p.y = y; p.life = 0.9f; p.col = col;
    strlcpy(p.txt, txt, sizeof(p.txt));
    return;
  }
}

static uint32_t multiplier() { return min<uint32_t>(1 + combo / 8, 5); }

void enterGame() {
  memset(meteors, 0, sizeof(meteors));
  memset(parts, 0, sizeof(parts));
  memset(popups, 0, sizeof(popups));
  gs = GS_READY;
  shieldA = -HALF_PI;
  shieldHalf = SHIELD_HALF;
  score = combo = 0;
  level = 1;
  lives = 3;
  spawnTimer = 1.0f;
  wideTimer = hitFlash = shakeT = playTime = overTimer = 0;
  newRecord = false;
}

static void spawnMeteor() {
  for (auto &m : meteors) {
    if (m.alive) continue;
    MeteorType t = M_ROCK;
    int r = random(100);
    if (r < 6) t = M_GOLD;
    else if (r < 10 && lives < MAX_LIVES) t = M_HEART;
    else if (level >= 3 && r < 10 + min<int>(level * 3, 30)) t = M_FAST;

    float base = 34 + level * 5.5f;
    m.alive = true;
    m.passed = false;
    m.type = t;
    m.a = frand(0, TWO_PI);
    m.r = SPAWN_R;
    m.spd = base * frand(0.85f, 1.15f) * (t == M_FAST ? 1.7f : (t == M_HEART ? 0.8f : 1.0f));
    m.size = (t == M_FAST) ? 3 : (t == M_ROCK ? frand(4, 6) : 5);
    return;
  }
}

static void gameOver() {
  gs = GS_OVER;
  overTimer = 0;
  gamesPlayed++;
  if (score > bestScore) { bestScore = score; newRecord = true; }
  saveStats();
  burst(CX, CY, TFT_ORANGE, 40, 140);
  burst(CX, CY, TFT_RED, 30, 90);
}

static void updateGame(float dt) {
  // --- Kalkan kontrolu ---
  if (touch.down && touchDist() > 12) {
    float target = touchAngle();
    shieldA += angDiff(target, shieldA) * min(1.0f, dt * 22.0f);
  }
  if (wideTimer > 0) wideTimer -= dt;
  float wantHalf = wideTimer > 0 ? SHIELD_HALF_WIDE : SHIELD_HALF;
  shieldHalf += (wantHalf - shieldHalf) * min(1.0f, dt * 8);

  playTime += dt;
  uint32_t newLevel = 1 + score / 15;
  if (newLevel > level) {
    level = newLevel;
    char buf[12];
    snprintf(buf, sizeof(buf), "SEVIYE %lu", (unsigned long)level);
    popup(CX, CY + 40, buf, C_CYAN);
  }

  // --- Dogurma ---
  spawnTimer -= dt;
  if (spawnTimer <= 0) {
    spawnMeteor();
    if (level >= 5 && random(100) < 15) spawnMeteor();  // ikili dalga
    spawnTimer = max(0.32f, 1.35f - level * 0.085f) * frand(0.7f, 1.2f);
  }

  // --- Meteorlar ---
  for (auto &m : meteors) {
    if (!m.alive) continue;
    m.r -= m.spd * dt;
    float mx = CX + cosf(m.a) * m.r, my = CY + sinf(m.a) * m.r;
    uint16_t col = meteorColor(m.type);

    // Kalkan bolgesi
    if (!m.passed && m.r - m.size <= SHIELD_R + 3) {
      if (fabsf(angDiff(m.a, shieldA)) <= shieldHalf + 0.06f) {
        m.alive = false;
        burst(mx, my, col, 12, 110);
        char buf[12];
        switch (m.type) {
          case M_HEART:
            popup(mx, my, "kalp kirildi", C_HEART);
            break;
          case M_GOLD: {
            combo++;
            uint32_t pts = 5 * multiplier();
            score += pts;
            wideTimer = 6.0f;
            snprintf(buf, sizeof(buf), "+%lu GENIS", (unsigned long)pts);
            popup(mx, my, buf, C_GOLD);
            totalBlocks++;
            break;
          }
          default: {
            combo++;
            uint32_t pts = (m.type == M_FAST ? 2 : 1) * multiplier();
            score += pts;
            snprintf(buf, sizeof(buf), "+%lu", (unsigned long)pts);
            popup(mx, my, buf, multiplier() > 1 ? C_GOLD : TFT_WHITE);
            totalBlocks++;
            break;
          }
        }
        continue;
      }
      if (m.r < SHIELD_R - 4) m.passed = true;
    }

    // Gezegene carpma
    if (m.r <= PLANET_R + m.size) {
      m.alive = false;
      if (m.type == M_HEART) {
        if (lives < MAX_LIVES) lives++;
        popup(CX, CY - 30, "+1 CAN", C_HEART);
        burst(mx, my, C_HEART, 16, 80);
      } else if (m.type == M_GOLD) {
        popup(CX, CY - 30, "kacti!", C_GOLD);
      } else {
        lives--;
        combo = 0;
        hitFlash = 0.25f;
        shakeT = 0.3f;
        burst(mx, my, TFT_ORANGE, 22, 120);
        if (lives == 0) { gameOver(); return; }
      }
    }
  }
}

static void updateFx(float dt) {
  for (auto &p : parts) {
    if (!p.alive) continue;
    p.x += p.vx * dt; p.y += p.vy * dt;
    p.vx *= 0.96f; p.vy *= 0.96f;
    p.life -= dt;
    if (p.life <= 0) p.alive = false;
  }
  for (auto &p : popups) {
    if (!p.alive) continue;
    p.y -= 28 * dt;
    p.life -= dt;
    if (p.life <= 0) p.alive = false;
  }
  if (hitFlash > 0) hitFlash -= dt;
  if (shakeT > 0) shakeT -= dt;
}

static void drawGame(float dt) {
  int ox = 0, oy = 0;
  if (shakeT > 0) { ox = random(-4, 5); oy = random(-4, 5); }
  int cx = CX + ox, cy = CY + oy;

  spr.fillSprite(hitFlash > 0 ? 0x3000 : C_BG);
  drawStars(dt, gs == GS_PLAY ? 1.0f + level * 0.4f : 1.0f);

  // Kalkan yolu
  spr.drawCircle(cx, cy, SHIELD_R, C_RING);

  // Gezegen
  if (gs != GS_OVER) {
    uint16_t pc = hitFlash > 0 ? TFT_RED : C_PLANET;
    spr.fillSmoothCircle(cx, cy, PLANET_R + 3, 0x0849, C_BG);
    spr.fillSmoothCircle(cx, cy, PLANET_R, pc, 0x0849);
    if (hitFlash <= 0) {
      spr.fillCircle(cx - 5, cy - 4, 5, C_LAND);
      spr.fillCircle(cx + 6, cy + 5, 4, C_LAND);
      spr.fillCircle(cx + 3, cy - 9, 2, C_LAND);
    }
  }

  // Meteorlar
  for (auto &m : meteors) {
    if (!m.alive) continue;
    float ca = cosf(m.a), sa = sinf(m.a);
    int mx = cx + ca * m.r, my = cy + sa * m.r;
    uint16_t col = meteorColor(m.type);
    switch (m.type) {
      case M_HEART:
        drawHeart(mx, my, 6, col);
        break;
      case M_GOLD:
        spr.drawCircle(mx, my, 8 + (millis() / 120) % 3, 0x6300);
        spr.fillSmoothCircle(mx, my, 5, col, C_BG);
        break;
      default: {
        float tail = m.type == M_FAST ? 18 : 9;
        spr.drawWideLine(mx, my, cx + ca * (m.r + tail), cy + sa * (m.r + tail),
                         m.size, m.type == M_FAST ? 0x6000 : 0x4A02, C_BG);
        spr.fillSmoothCircle(mx, my, m.size, col, C_BG);
        break;
      }
    }
  }

  // Kalkan
  if (gs != GS_OVER) {
    uint16_t sc = wideTimer > 0 ? C_GOLD : C_SHIELD;
    spr.drawArc(cx, cy, SHIELD_R + 4, SHIELD_R - 3, toArcDeg(shieldA - shieldHalf),
                toArcDeg(shieldA + shieldHalf), sc, C_BG, true);
  }

  // Parcaciklar
  for (auto &p : parts) {
    if (!p.alive) continue;
    spr.fillRect(p.x + ox, p.y + oy, 2, 2, p.col);
  }

  // Popuplar
  spr.setTextFont(2);
  spr.setTextDatum(MC_DATUM);
  for (auto &p : popups) {
    if (!p.alive) continue;
    spr.setTextColor(p.col);
    spr.drawString(p.txt, p.x, p.y);
  }

  // HUD
  if (gs == GS_PLAY) {
    spr.setTextFont(4);
    spr.setTextColor(TFT_WHITE);
    spr.drawNumber(score, CX, 26);
    if (multiplier() > 1) {
      spr.setTextFont(2);
      spr.setTextColor(C_GOLD);
      spr.drawString(String("x") + multiplier(), CX, 46);
    }
    for (int i = 0; i < lives; i++) {
      drawHeart(CX - (lives - 1) * 9 + i * 18, 214, 5, TFT_RED);
    }
  }

  if (gs == GS_READY) {
    spr.setTextFont(4);
    spr.setTextColor(C_CYAN);
    spr.drawString("ORBIT GUARD", CX, 42);
    spr.setTextFont(2);
    spr.setTextColor(TFT_WHITE);
    spr.drawString("Dokun: kalkan o yone doner", CX, 172);
    spr.setTextColor(C_HEART);
    spr.drawString("Yesil kalp: iceri birak", CX, 189);
    spr.setTextColor(C_GOLD);
    spr.drawString("Altin: yakala!", CX, 206);
    if ((millis() / 500) % 2) {
      spr.setTextColor(TFT_WHITE);
      spr.drawString("baslamak icin dokun", CX, 66);
    }
  }

  if (gs == GS_OVER) {
    spr.setTextFont(4);
    spr.setTextColor(TFT_RED);
    spr.drawString("OYUN BITTI", CX, 62);
    spr.setTextColor(TFT_WHITE);
    spr.setTextFont(7);
    spr.drawNumber(score, CX, 110);
    spr.setTextFont(2);
    if (newRecord && (millis() / 300) % 2) {
      spr.setTextColor(C_GOLD);
      spr.drawString("YENI REKOR!", CX, 145);
    } else if (!newRecord) {
      spr.setTextColor(C_DIM);
      spr.drawString(String("Rekor: ") + bestScore, CX, 145);
    }
    if (overTimer > 0.8f) {
      spr.fillRoundRect(42, 162, 74, 32, 16, 0x0400);
      spr.fillRoundRect(124, 162, 74, 32, 16, 0x2945);
      spr.setTextColor(TFT_WHITE);
      spr.drawString("TEKRAR", 79, 178);
      spr.drawString("MENU", 161, 178);
    }
  }

  drawToast();
  spr.pushSprite(0, 0);
}

void loopGame(float dt) {
  switch (gs) {
    case GS_READY:
      if (touch.down && touchDist() > 12) shieldA += angDiff(touchAngle(), shieldA) * min(1.0f, dt * 22);
      if (touch.pressed) gs = GS_PLAY;
      break;
    case GS_PLAY:
      updateGame(dt);
      break;
    case GS_OVER:
      overTimer += dt;
      if (overTimer > 0.8f && touch.pressed && touch.y > 150 && touch.y < 205) {
        if (touch.x < CX) { enterGame(); gs = GS_PLAY; waitForRelease = true; }
        else { switchApp(APP_MENU); return; }
      }
      break;
  }
  updateFx(dt);
  drawGame(dt);
}

// ---------------------------------------------------------------------------
// CIZIM (dokunmatik test) - dogrudan ekrana cizer, sprite kullanmaz
// ---------------------------------------------------------------------------
static const int PAL_SLOTS = 10;
static const float PAL_R = 106, DRAW_R = 92;
const uint16_t palColors[PAL_SLOTS] = {
  0, TFT_RED, TFT_ORANGE, TFT_YELLOW, TFT_GREEN,   // 0 = temizle
  0, C_CYAN, TFT_BLUE, TFT_MAGENTA, TFT_WHITE      // 5 = cikis
};
int palSel = 9;
bool paintHasLast = false;
int16_t paintLX, paintLY;

static float slotAngle(int i) { return -HALF_PI + i * TWO_PI / PAL_SLOTS; }

static void drawPalette() {
  for (int i = 0; i < PAL_SLOTS; i++) {
    int x = CX + cosf(slotAngle(i)) * PAL_R, y = CY + sinf(slotAngle(i)) * PAL_R;
    tft.fillCircle(x, y, 12, TFT_BLACK);
    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    if (i == 0) {
      tft.drawCircle(x, y, 9, TFT_WHITE);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("C", x, y);
    } else if (i == 5) {
      tft.drawCircle(x, y, 9, TFT_RED);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("X", x, y);
    } else {
      tft.fillCircle(x, y, 9, palColors[i]);
      if (i == palSel) tft.drawCircle(x, y, 12, TFT_WHITE);
    }
  }
}

static void clearCanvas() {
  tft.fillScreen(TFT_BLACK);
  tft.drawCircle(CX, CY, DRAW_R + 2, C_RING);
  drawPalette();
}

void enterPaint() {
  paintHasLast = false;
  clearCanvas();
}

void loopPaint() {
  if (!touch.down) { paintHasLast = false; return; }
  float d = touchDist();

  if (d > DRAW_R + 4) {
    paintHasLast = false;
    if (!touch.pressed) return;
    float a = touchAngle() + HALF_PI;  // slot 0 ustte
    while (a < 0) a += TWO_PI;
    int slot = (int)lroundf(a / (TWO_PI / PAL_SLOTS)) % PAL_SLOTS;
    if (slot == 0) clearCanvas();
    else if (slot == 5) switchApp(APP_MENU);
    else { palSel = slot; drawPalette(); }
    return;
  }

  uint16_t col = palColors[palSel];
  if (paintHasLast) {
    tft.drawWideLine(paintLX, paintLY, touch.x, touch.y, 5, col, TFT_BLACK);
  } else {
    tft.fillSmoothCircle(touch.x, touch.y, 3, col, TFT_BLACK);
  }
  paintLX = touch.x;
  paintLY = touch.y;
  paintHasLast = true;
}

// ---------------------------------------------------------------------------
// FOTOGRAF (internetten indirilen JPEG'ler, kaydirmali karusel)
// ---------------------------------------------------------------------------
static const int MAX_PHOTOS = 16;
static const size_t JPG_CAP = 120 * 1024;
const int photoCount = min<int>(sizeof(PHOTO_URLS) / sizeof(PHOTO_URLS[0]), MAX_PHOTOS);

enum PhotoState : uint8_t { PH_WAIT, PH_LOADING, PH_READY, PH_ERROR };
uint16_t *photoBuf[MAX_PHOTOS];          // PSRAM'de 240x240 RGB565 (ekran bayt sirasinda)
volatile uint8_t photoState[MAX_PHOTOS];
bool photoTaskStarted = false;

int photoIdx = 0;
float viewX = 0;                         // karuselin piksel cinsinden konumu
bool photoDragging = false;
int16_t dragStartX, dragStartY, dragPrevX;
uint32_t dragStartMs;
float dragVel = 0;                       // px/s, birakinca "firlatma" icin
bool photoAuto = false;
uint32_t photoAutoMs = 0, photoHintUntil = 0;

// HTTPClient::writeToStream icin bellege yazan basit Stream
class BufStream : public Stream {
 public:
  uint8_t *buf; size_t cap, len = 0;
  BufStream(uint8_t *b, size_t c) : buf(b), cap(c) {}
  size_t write(uint8_t c) override { if (len >= cap) return 0; buf[len++] = c; return 1; }
  size_t write(const uint8_t *d, size_t n) override {
    n = min(n, cap - len);
    memcpy(buf + len, d, n);
    len += n;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

static uint16_t *decodeTarget = nullptr;

static bool jpgToBuffer(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
  for (int r = 0; r < h; r++) {
    int yy = y + r;
    if (yy < 0 || yy >= H) continue;
    int x0 = max<int>(0, x), x1 = min<int>(W, x + w);
    if (x1 > x0) memcpy(decodeTarget + yy * W + x0, bmp + r * w + (x0 - x), (x1 - x0) * 2);
  }
  return true;
}

static bool downloadPhoto(int i, uint8_t *jpg) {
  WiFiClientSecure client;
  client.setInsecure();  // demo: sertifika dogrulamasi yok
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(client, PHOTO_URLS[i])) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Foto %d: HTTP %d\n", i, code);
    http.end();
    return false;
  }
  BufStream bs(jpg, JPG_CAP);
  int n = http.writeToStream(&bs);
  http.end();
  if (n <= 0 || bs.len == 0) {
    Serial.printf("Foto %d: indirme hatasi %d\n", i, n);
    return false;
  }

  if (!photoBuf[i]) photoBuf[i] = (uint16_t *)ps_malloc(W * H * 2);
  if (!photoBuf[i]) return false;
  memset(photoBuf[i], 0, W * H * 2);

  uint16_t jw = 0, jh = 0;
  TJpgDec.getJpgSize(&jw, &jh, jpg, bs.len);
  uint8_t scale = 1;
  while (scale < 8 && (jw / scale > W * 1.5f || jh / scale > H * 1.5f)) scale *= 2;
  TJpgDec.setJpgScale(scale);
  decodeTarget = photoBuf[i];
  JRESULT res = TJpgDec.drawJpg((W - jw / scale) / 2, (H - jh / scale) / 2, jpg, bs.len);
  Serial.printf("Foto %d: %u bayt, %ux%u, sonuc %d\n", i, (unsigned)bs.len, jw, jh, res);
  return res == JDR_OK;
}

// Indirme ayri bir gorevde (core 0) calisir; arayuz donmaz
static void photoTask(void *) {
  uint8_t *jpg = (uint8_t *)ps_malloc(JPG_CAP);
  TJpgDec.setSwapBytes(true);  // sprite bellegiyle ayni bayt sirasi
  TJpgDec.setCallback(jpgToBuffer);
  for (int pass = 0; pass < 3 && jpg; pass++) {
    bool anyFail = false;
    for (int i = 0; i < photoCount; i++) {
      if (photoState[i] == PH_READY) continue;
      while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));
      photoState[i] = PH_LOADING;
      photoState[i] = downloadPhoto(i, jpg) ? PH_READY : PH_ERROR;
      if (photoState[i] != PH_READY) anyFail = true;
    }
    if (!anyFail) break;
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
  free(jpg);
  vTaskDelete(nullptr);
}

void enterPhoto() {
  photoDragging = false;
  viewX = photoIdx * W;
  photoHintUntil = millis() + 3000;
}

// Bir fotoyu sprite'a sx konumundan kopyalar (yatay kirpma ile)
static void blitPhoto(int i, int sx) {
  int x0 = max(0, sx), x1 = min<int>(W, sx + W);
  if (x1 <= x0) return;
  int cx = sx + W / 2;
  if (photoState[i] == PH_READY) {
    uint16_t *dst = (uint16_t *)spr.getPointer();
    const uint16_t *src = photoBuf[i];
    for (int y = 0; y < H; y++) memcpy(dst + y * W + x0, src + y * W + (x0 - sx), (x1 - x0) * 2);
    return;
  }
  spr.setTextFont(2);
  spr.setTextDatum(MC_DATUM);
  if (photoState[i] == PH_ERROR) {
    spr.setTextColor(TFT_RED);
    spr.drawString("Yuklenemedi", cx, CY);
  } else {
    float a = millis() / 180.0f;
    spr.drawArc(cx, CY, 22, 18, toArcDeg(a), toArcDeg(a + 2.0f), C_CYAN, C_BG, true);
    spr.setTextColor(C_DIM);
    spr.drawString(photoState[i] == PH_LOADING ? "Indiriliyor..." : "Sirada", cx, CY + 40);
  }
}

static void photoGoTo(int idx) {
  photoIdx = constrain(idx, 0, photoCount - 1);
  photoAutoMs = millis();
}

void loopPhoto(float dt) {
  // --- Gerekirse indirme gorevini baslat ---
  if (!photoTaskStarted && psramFound() && wifiEnabled && WiFi.status() == WL_CONNECTED) {
    photoTaskStarted = true;
    xTaskCreatePinnedToCore(photoTask, "photos", 16384, nullptr, 1, nullptr, 0);
  }

  // --- Dokunmatik: kaydirma / dokunma / asagi cekme ---
  if (touch.pressed) {
    photoDragging = true;
    dragStartX = dragPrevX = touch.x;
    dragStartY = touch.y;
    dragStartMs = millis();
    dragVel = 0;
  }
  if (photoDragging && touch.down) {
    int dx = touch.x - dragStartX;
    if (dt > 0) dragVel = dragVel * 0.6f + 0.4f * ((touch.x - dragPrevX) / dt);
    dragPrevX = touch.x;
    float x = photoIdx * W - dx;
    float maxX = (photoCount - 1) * W;
    if (x < 0) x *= 0.35f;                        // uclarda lastik efekti
    else if (x > maxX) x = maxX + (x - maxX) * 0.35f;
    viewX = x;
  }
  if (photoDragging && touch.released) {
    photoDragging = false;
    int dx = touch.x - dragStartX, dy = touch.y - dragStartY;
    if (dy > 70 && abs(dy) > abs(dx) * 2) { switchApp(APP_MENU); return; }
    if (abs(dx) < 10 && abs(dy) < 10 && millis() - dragStartMs < 350) {
      photoAuto = !photoAuto;                     // dokun: otomatik oynat ac/kapa
      photoAutoMs = millis();
    } else if (dx < -50 || dragVel < -500) photoGoTo(photoIdx + 1);
    else if (dx > 50 || dragVel > 500) photoGoTo(photoIdx - 1);
  }

  // --- Otomatik slayt ---
  if (photoAuto && !photoDragging && millis() - photoAutoMs > 4000) {
    photoGoTo((photoIdx + 1) % photoCount);
  }

  // --- Animasyon ---
  float target = photoIdx * W;
  if (!photoDragging) {
    viewX += (target - viewX) * min(1.0f, dt * 12.0f);
    if (fabsf(target - viewX) < 0.5f) viewX = target;
  }

  // --- Cizim ---
  spr.fillSprite(C_BG);
  if (!psramFound()) {
    spr.setTextFont(2);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_RED);
    spr.drawString("PSRAM bulunamadi", CX, CY - 10);
    spr.setTextColor(C_DIM);
    spr.drawString("Arduino IDE: PSRAM = OPI", CX, CY + 10);
  } else if (!wifiEnabled || (WiFi.status() != WL_CONNECTED && !photoTaskStarted)) {
    spr.setTextFont(2);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_ORANGE);
    spr.drawString(wifiEnabled ? "WiFi bekleniyor..." : "WiFi gerekli (config.h)", CX, CY);
  } else {
    int first = (int)floorf(viewX / W);
    for (int i = first; i <= first + 1; i++) {
      if (i < 0 || i >= photoCount) continue;
      blitPhoto(i, i * W - (int)lroundf(viewX));
    }
  }

  // Sayfa noktalari
  int dotsW = (photoCount - 1) * 11;
  for (int i = 0; i < photoCount; i++) {
    int x = CX - dotsW / 2 + i * 11;
    spr.fillSmoothCircle(x, 224, i == photoIdx ? 4 : 3, i == photoIdx ? TFT_WHITE : 0x5AEB, TFT_BLACK);
  }
  spr.setTextFont(2);
  spr.setTextDatum(MC_DATUM);
  if (photoAuto) {
    spr.setTextColor(TFT_WHITE);
    spr.drawString("OTO", CX, 16);
  }
  if (millis() < photoHintUntil) {
    spr.fillRoundRect(35, 168, 170, 40, 12, 0x18E3);
    spr.setTextColor(TFT_WHITE);
    spr.drawString("< kaydir >   dokun: oto", CX, 180);
    spr.drawString("asagi cek: menu", CX, 196);
  }

  drawToast();
  spr.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// SAAT
// ---------------------------------------------------------------------------
void enterClock() {}

static void hand(float a, float len, float tail, float w, uint16_t col) {
  float c = cosf(a), s = sinf(a);
  spr.drawWideLine(CX - c * tail, CY - s * tail, CX + c * len, CY + s * len, w, col, C_BG);
}

void loopClock(float dt) {
  if (touch.pressed) { switchApp(APP_MENU); return; }

  spr.fillSprite(C_BG);
  drawStars(dt, 0.5f);

  // Kadran
  for (int i = 0; i < 60; i++) {
    float a = i * TWO_PI / 60;
    bool big = i % 5 == 0;
    float r1 = big ? 100 : 106, r2 = 112;
    spr.drawWideLine(CX + cosf(a) * r1, CY + sinf(a) * r1, CX + cosf(a) * r2, CY + sinf(a) * r2,
                     big ? 3 : 1, big ? TFT_WHITE : C_DIM, C_BG);
  }
  spr.setTextFont(4);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(C_CYAN);
  spr.drawString("12", CX, 33);
  spr.drawString("3", 207, CY);
  spr.drawString("6", CX, 207);
  spr.drawString("9", 33, CY);

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tmv;
  time_t t = tv.tv_sec;
  localtime_r(&t, &tmv);

  if (tmv.tm_year + 1900 < 2024) {
    spr.setTextFont(2);
    spr.setTextColor(TFT_ORANGE);
    if (!wifiEnabled) spr.drawString("WiFi ayarlanmamis", CX, CY);
    else spr.drawString("Saat aliniyor...", CX, CY);
  } else {
    float sec = tmv.tm_sec + tv.tv_usec / 1e6f;
    float minF = tmv.tm_min + sec / 60.0f;
    float hrF = (tmv.tm_hour % 12) + minF / 60.0f;

    char buf[16];
    spr.setTextFont(2);
    spr.setTextColor(TFT_WHITE);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
    spr.drawString(buf, CX, 80);
    static const char *days[] = {"Pazar", "Pazartesi", "Sali", "Carsamba", "Persembe", "Cuma", "Cumartesi"};
    spr.setTextColor(C_DIM);
    snprintf(buf, sizeof(buf), "%02d.%02d.%04d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900);
    spr.drawString(buf, CX, 158);
    spr.drawString(days[tmv.tm_wday], CX, 174);

    hand(hrF / 12.0f * TWO_PI - HALF_PI, 52, 8, 7, TFT_WHITE);
    hand(minF / 60.0f * TWO_PI - HALF_PI, 80, 10, 4, C_CYAN);
    hand(sec / 60.0f * TWO_PI - HALF_PI, 92, 18, 2, TFT_RED);
    spr.fillSmoothCircle(CX, CY, 5, TFT_RED, C_BG);
  }

  drawToast();
  spr.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Web paneli
// ---------------------------------------------------------------------------
static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="tr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>XIAO Arcade</title>
<style>
body{font-family:system-ui,sans-serif;background:#0b0f1a;color:#e8eefc;max-width:480px;margin:auto;padding:20px}
h1{color:#3ff;margin-bottom:4px}.card{background:#151c2e;border-radius:14px;padding:16px;margin:14px 0}
.big{font-size:42px;font-weight:700;color:#fd0}input{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:0;font-size:16px}
button{margin-top:10px;padding:12px 18px;border:0;border-radius:10px;background:#3ff;color:#000;font-weight:700;font-size:16px}
.muted{color:#8796b0}.danger{background:#f55;color:#fff}
</style></head><body>
<h1>XIAO Arcade</h1><div class="muted">Orbit Guard istatistikleri</div>
<div class="card"><div class="muted">Rekor</div><div class="big" id="best">-</div>
<div>Oynanan oyun: <b id="plays">-</b> &nbsp; Engellenen meteor: <b id="blocks">-</b></div></div>
<div class="card"><b>Ekrana mesaj gonder</b>
<input id="msg" maxlength="30" placeholder="Merhaba!">
<button onclick="send()">Gonder</button></div>
<div class="card"><button class="danger" onclick="if(confirm('Rekor sifirlansin mi?'))fetch('/reset',{method:'POST'}).then(load)">Rekoru sifirla</button></div>
<script>
function load(){fetch('/api').then(r=>r.json()).then(j=>{best.textContent=j.best;plays.textContent=j.plays;blocks.textContent=j.blocks})}
function send(){fetch('/msg',{method:'POST',body:new URLSearchParams({t:msg.value})});msg.value=''}
load();setInterval(load,3000);
</script></body></html>)HTML";

static void setupWeb() {
  server.on("/", HTTP_GET, [] { server.send(200, "text/html", PAGE); });
  server.on("/api", HTTP_GET, [] {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"best\":%lu,\"plays\":%lu,\"blocks\":%lu}",
             (unsigned long)bestScore, (unsigned long)gamesPlayed, (unsigned long)totalBlocks);
    server.send(200, "application/json", buf);
  });
  server.on("/msg", HTTP_POST, [] {
    toAsciiTR(server.arg("t"), toastText, 27);
    toastUntil = millis() + 6000;
    Serial.printf("Mesaj: %s\n", toastText);
    server.send(200, "text/plain", "ok");
  });
  server.on("/reset", HTTP_POST, [] {
    bestScore = 0;
    saveStats();
    server.send(200, "text/plain", "ok");
  });
  server.begin();
}

static void handleNetwork() {
  if (!wifiEnabled) return;
  if (!netStarted && WiFi.status() == WL_CONNECTED) {
    netStarted = true;
    Serial.printf("WiFi baglandi: %s\n", WiFi.localIP().toString().c_str());
    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
    if (MDNS.begin(DEVICE_HOSTNAME)) MDNS.addService("http", "tcp", 80);
    setupWeb();
    snprintf(toastText, sizeof(toastText), "WiFi: %s", WiFi.localIP().toString().c_str());
    toastUntil = millis() + 4000;
  }
  if (netStarted) server.handleClient();
}

// ---------------------------------------------------------------------------
// Kurulum ve ana dongu
// ---------------------------------------------------------------------------
static void splash() {
  for (int r = 0; r <= 120; r += 6) {
    tft.drawCircle(CX, CY, r, tft.color565(0, r * 2, r * 2));
    delay(8);
  }
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(C_CYAN, TFT_BLACK);
  tft.drawString("XIAO ARCADE", CX, CY);
  delay(700);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nXIAO Arcade basliyor");

  tft.init();  // Seeed_GFX: Wire.begin() ve TOUCH_INT pinini de hazirlar
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  pinMode(TOUCH_INT, INPUT_PULLUP);

  spr.setColorDepth(16);
  spr.setAttribute(PSRAM_ENABLE, false);  // hiz icin dahili RAM (115 KB)
  if (!spr.createSprite(W, H)) {
    spr.setAttribute(PSRAM_ENABLE, true);
    if (!spr.createSprite(W, H)) Serial.println("HATA: sprite olusturulamadi!");
  }

  randomSeed(esp_random());
  initStars();

  prefs.begin("arcade", false);
  bestScore = prefs.getUInt("best", 0);
  gamesPlayed = prefs.getUInt("plays", 0);
  totalBlocks = prefs.getUInt("blocks", 0);

  wifiEnabled = strlen(WIFI_SSID) > 0 && strcmp(WIFI_SSID, "WIFI_ADINIZ") != 0;
  if (wifiEnabled) {
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  splash();
  switchApp(APP_MENU);
}

void loop() {
  static uint32_t lastFrame = millis();
  handleNetwork();

  if (app == APP_PAINT) {  // cizim: kare siniri yok, hizli ornekleme
    updateTouch();
    loopPaint();
    return;
  }

  uint32_t now = millis();
  if (now - lastFrame < 16) return;  // ~60 FPS ust sinir
  float dt = min((now - lastFrame) / 1000.0f, 0.05f);
  lastFrame = now;
  updateTouch();  // kare basina bir kez: pressed/released olaylari kaybolmasin

  switch (app) {
    case APP_MENU:  loopMenu(dt);  break;
    case APP_GAME:  loopGame(dt);  break;
    case APP_CLOCK: loopClock(dt); break;
    case APP_PHOTO: loopPhoto(dt); break;
    default: break;
  }
}
