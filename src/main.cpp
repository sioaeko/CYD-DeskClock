/*
 * CYD 데스크 시계 — ESP32-2432S028R (Cheap Yellow Display, 2.8" ILI9341 + XPT2046)
 *
 * 기능
 *  - WiFiManager 캡티브 포털로 WiFi 설정 (핫스팟: CYD-Clock)
 *  - NTP 시간 동기화 (한국 표준시), 이후 자동 재동기화
 *  - 큰 시계 + 한글 날짜/요일 + 초 진행 바
 *  - 날씨/기온/습도 표시 (Open-Meteo, 무료·API 키 불필요, IP 기반 위치 자동)
 *  - 조도센서(LDR) 자동 밝기 조절
 *  - 터치: 왼쪽 탭 = 12/24시간제 전환, 가운데 탭 = 테마 변경, 오른쪽 탭 = 밝기 모드 전환,
 *          5초 길게 누르기 = WiFi 설정 초기화
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ================= 사용자 설정 =================
static const char* TZ_INFO = "KST-9";            // 한국 표준시 (변경: https://gist.github.com/alwynallan/24d96091655391107939 참고)
static const char* NTP_1   = "kr.pool.ntp.org";
static const char* NTP_2   = "time.google.com";
static const char* NTP_3   = "pool.ntp.org";
static const char* AP_NAME = "CYD-Clock";        // 최초 WiFi 설정용 핫스팟 이름
static const char* HOSTNAME = "cyd-clock";
static const uint8_t CLOCK_ROTATION = 6;         // TPM408/CYD v2 가로 방향. 뒤집히면 4로 변경.

// 날씨: 기본은 IP 기반 자동 위치. 특정 좌표로 고정하려면 값을 넣으세요 (예: 서울 37.5665, 126.9780).
static float MANUAL_LAT = 0.0f;                  // 0 = 자동
static float MANUAL_LON = 0.0f;

// 화면 색이 반전되어 보이는 CYD 변종(배경이 하얗게 나옴)이라면 true 로 바꾸세요.
#define PANEL_INVERT false

// 조도센서: 밝을수록 ADC 값이 낮아지는 배선 기준. 반대로 동작하면 false 로.
#define LDR_BRIGHT_IS_LOW true
static const int LDR_RAW_BRIGHT = 200;    // 밝은 방에서의 대략적인 ADC 값
static const int LDR_RAW_DARK   = 3400;   // 어두운 방에서의 대략적인 ADC 값
static const int BRIGHTNESS_MIN = 10;     // 자동 모드 최소 밝기 (0~255)
static const int BRIGHTNESS_MAX = 255;

// ================= 핀 정의 (CYD 고정) =================
static const int PIN_LDR   = 34;
static const int PIN_LED_R = 4;
static const int PIN_LED_G = 16;
static const int PIN_LED_B = 17;

// ================= 디스플레이 설정 =================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX() {
    { // 디스플레이 SPI 버스 (HSPI)
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 14;
      cfg.pin_mosi    = 13;
      cfg.pin_miso    = 12;
      cfg.pin_dc      = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // 패널: TPM408/CYD v2는 ILI9341 호환 초기화 + 320x240 네이티브 레이아웃입니다.
      auto cfg = _panel.config();
      cfg.pin_cs           = 15;
      cfg.pin_rst          = -1;
      cfg.pin_busy         = -1;
      cfg.memory_width     = 320;
      cfg.memory_height    = 240;
      cfg.panel_width      = 320;
      cfg.panel_height     = 240;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = true;
      cfg.invert           = PANEL_INVERT;
      cfg.rgb_order        = true;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }
    { // 백라이트 PWM
      auto cfg = _light.config();
      cfg.pin_bl      = 21;
      cfg.invert      = false;
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    { // 터치 (XPT2046, 별도 SPI 핀)
      auto cfg = _touch.config();
      cfg.x_min           = 300;
      cfg.x_max           = 3900;
      cfg.y_min           = 3700;
      cfg.y_max           = 200;
      cfg.pin_int         = -1;
      cfg.bus_shared      = false;
      cfg.offset_rotation = 2;
      cfg.spi_host        = -1;          // software SPI: CYD 변종에서 가장 호환성이 좋음
      cfg.freq            = 1000000;
      cfg.pin_sclk        = 25;
      cfg.pin_mosi        = 32;
      cfg.pin_miso        = 39;
      cfg.pin_cs          = 33;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

static LGFX tft;
static LGFX_Sprite canvas(&tft);
static bool canvasOk = false;
static int16_t screenW = 320;
static int16_t screenH = 240;

static WiFiManager wm;
static Preferences prefs;
static WiFiManagerParameter wifiHint(
  "<p style='font-size:13px;color:#666'>ESP32는 2.4GHz WiFi만 지원합니다. "
  "공유기가 WPA3-only이면 WPA2/WPA3 혼합 또는 WPA2로 바꿔 주세요.</p>");
static bool wifiHintAdded = false;

// ================= 상태 =================
static bool    use12h     = false;
static uint8_t brightMode = 0;              // 0=자동 1=최대 2=중간 3=최소
static uint8_t themeIndex = 0;
static const uint8_t BRIGHT_FIXED[4] = { 0, 255, 110, 25 };
static const char*   BRIGHT_NAME[4]  = { "자동", "최대", "중간", "최소" };

struct Theme {
  const char* name;
  uint8_t bgTop[3], bgBottom[3], panel[3], fg[3], muted[3], faint[3], accent[3], accent2[3], warn[3];
};

static const Theme THEMES[] = {
  { "네온", { 8, 12, 24 }, { 0, 0, 0 }, { 13, 17, 28 }, { 245, 248, 252 }, { 150, 160, 178 }, { 84, 92, 108 }, { 0, 190, 255 }, { 95, 235, 185 }, { 255, 214, 90 } },
  { "앰버", { 26, 13, 7 }, { 2, 1, 0 }, { 24, 14, 8 }, { 255, 244, 222 }, { 188, 160, 128 }, { 104, 78, 54 }, { 255, 170, 58 }, { 255, 91, 74 }, { 255, 222, 120 } },
  { "포레스트", { 5, 24, 22 }, { 0, 4, 3 }, { 8, 25, 22 }, { 232, 250, 242 }, { 137, 174, 160 }, { 72, 104, 92 }, { 74, 222, 128 }, { 56, 189, 248 }, { 250, 204, 21 } },
  { "모노", { 15, 16, 20 }, { 0, 0, 0 }, { 18, 19, 24 }, { 250, 250, 250 }, { 160, 164, 172 }, { 86, 90, 102 }, { 220, 224, 232 }, { 118, 128, 142 }, { 255, 214, 90 } },
};
static const uint8_t THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);

static float    ldrEma        = 2000.0f;
static int      curBrightness = -1;
static char     toastMsg[64]  = "";
static uint32_t toastUntil    = 0;
static char     lastSig[320]  = "";

static const char* WEEKDAY_KR[7] = { "일요일", "월요일", "화요일", "수요일", "목요일", "금요일", "토요일" };

// 날씨 상태 (백그라운드 태스크가 갱신, 화면 루프가 읽음)
static volatile bool  weatherValid = false;
static volatile float weatherTemp  = 0.0f;
static volatile int   weatherHum   = 0;
static volatile int   weatherCode  = -1;
static char           weatherCity[24] = "";
static float          geoLat = 0.0f, geoLon = 0.0f;
static bool           geoResolved = false;

// 색상 (RGB888)
static inline uint32_t C(uint8_t r, uint8_t g, uint8_t b) { return lgfx::color888(r, g, b); }
static inline uint32_t TC(const uint8_t rgb[3]) { return C(rgb[0], rgb[1], rgb[2]); }

static uint32_t blendColor(const uint8_t from[3], const uint8_t to[3], int pos, int span) {
  int ratio = (span > 1) ? constrain(pos * 255 / (span - 1), 0, 255) : 0;
  int r = from[0] + ((int)to[0] - from[0]) * ratio / 255;
  int g = from[1] + ((int)to[1] - from[1]) * ratio / 255;
  int b = from[2] + ((int)to[2] - from[2]) * ratio / 255;
  return C((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// ================= 그리기 유틸 =================
template <typename GFX>
static void drawWifiIcon(GFX& g, int x, int y, uint32_t col) {
  g.fillCircle(x, y, 2, col);
  g.fillArc(x, y, 6, 4, 225, 315, col);
  g.fillArc(x, y, 11, 9, 225, 315, col);
}

static const char* weatherLabel(int code) {
  switch (code) {
    case 0:  return "맑음";
    case 1:  return "대체로 맑음";
    case 2:  return "구름 조금";
    case 3:  return "흐림";
    case 45: case 48: return "안개";
    case 51: case 53: case 55: return "이슬비";
    case 56: case 57: return "어는 이슬비";
    case 61: return "약한 비"; case 63: return "비"; case 65: return "강한 비";
    case 66: case 67: return "어는 비";
    case 71: return "약한 눈"; case 73: return "눈"; case 75: return "강한 눈";
    case 77: return "싸락눈";
    case 80: case 81: return "소나기"; case 82: return "강한 소나기";
    case 85: case 86: return "눈 소나기";
    case 95: return "뇌우"; case 96: case 99: return "우박 뇌우";
    default: return "";
  }
}

template <typename GFX>
static void drawCloudShape(GFX& g, int x, int y, uint32_t col) {
  g.fillCircle(x - 6, y + 2, 5, col);
  g.fillCircle(x + 6, y + 3, 5, col);
  g.fillCircle(x - 1, y - 3, 7, col);
  g.fillRoundRect(x - 10, y + 2, 21, 6, 3, col);
}

template <typename GFX>
static void drawSun(GFX& g, int x, int y, int r, uint32_t col) {
  g.fillCircle(x, y, r, col);
  const float dx[8] = { 1, 0.7f, 0, -0.7f, -1, -0.7f, 0, 0.7f };
  const float dy[8] = { 0, 0.7f, 1, 0.7f, 0, -0.7f, -1, -0.7f };
  for (int i = 0; i < 8; i++) {
    g.drawLine(x + (int)(dx[i] * (r + 2)), y + (int)(dy[i] * (r + 2)),
               x + (int)(dx[i] * (r + 5)), y + (int)(dy[i] * (r + 5)), col);
  }
}

// WMO 날씨 코드를 아이콘으로. code < 0 이면 아직 데이터 없음.
template <typename GFX>
static void drawWeatherIcon(GFX& g, int x, int y, int code, const Theme& theme) {
  const uint32_t sun   = C(255, 200, 60);
  const uint32_t cloud = C(205, 214, 228);
  const uint32_t rain  = TC(theme.accent);
  const uint32_t snow  = C(225, 238, 255);
  const uint32_t bolt  = C(255, 210, 70);
  if (code < 0) { g.drawCircle(x, y, 9, TC(theme.faint)); return; }

  int cat;
  if (code == 0) cat = 0;
  else if (code == 1 || code == 2) cat = 1;
  else if (code == 3) cat = 2;
  else if (code == 45 || code == 48) cat = 3;
  else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) cat = 4;
  else if ((code >= 71 && code <= 77) || code == 85 || code == 86) cat = 5;
  else if (code >= 95) cat = 6;
  else cat = 2;

  switch (cat) {
    case 0:
      drawSun(g, x, y, 7, sun);
      break;
    case 1:
      drawSun(g, x - 4, y - 4, 5, sun);
      drawCloudShape(g, x + 2, y + 3, cloud);
      break;
    case 2:
      drawCloudShape(g, x, y, cloud);
      break;
    case 3:
      drawCloudShape(g, x, y - 2, cloud);
      for (int i = 0; i < 3; i++) g.drawFastHLine(x - 9, y + 8 + i * 3, 18, TC(theme.muted));
      break;
    case 4:
      drawCloudShape(g, x, y - 2, cloud);
      for (int i = -1; i <= 1; i++) g.drawLine(x + i * 6, y + 7, x + i * 6 - 2, y + 12, rain);
      break;
    case 5:
      drawCloudShape(g, x, y - 2, cloud);
      for (int i = -1; i <= 1; i++) g.fillCircle(x + i * 6, y + 10, 1, snow);
      break;
    case 6:
      drawCloudShape(g, x, y - 2, cloud);
      g.fillTriangle(x - 1, y + 6, x + 3, y + 6, x - 2, y + 12, bolt);
      g.fillTriangle(x + 2, y + 8, x - 2, y + 14, x + 3, y + 8, bolt);
      break;
  }
}

template <typename GFX>
static void drawThemedBackground(GFX& g, const Theme& theme, int w, int h) {
  const int bandH = 4;
  for (int y = 0; y < h; y += bandH) {
    g.fillRect(0, y, w, bandH, blendColor(theme.bgTop, theme.bgBottom, y, h));
  }
  // 얇은 상단 강조선 하나만 — 나머지는 여백으로 비워 깔끔하게.
  g.fillRect(0, 0, w, 2, TC(theme.accent));
}

static void setLandscapeRotation() {
  const uint8_t rotationFlags = CLOCK_ROTATION & 4;
  const uint8_t rotationBase = CLOCK_ROTATION & 3;
  for (uint8_t i = 0; i < 4; ++i) {
    uint8_t rotation = rotationFlags | ((rotationBase + i) & 3);
    tft.setRotation(rotation);
    if (tft.width() >= tft.height()) break;
  }
  screenW = tft.width();
  screenH = tft.height();
}

// 부팅/설정 단계용 전체 화면 메시지
static void drawScreen(const char* title, const char* line1, const char* line2 = nullptr, const char* line3 = nullptr) {
  const int w = tft.width();
  const int h = tft.height();
  const int cx = w / 2;
  const int titleY = h * 29 / 100;
  const int ruleW = min(80, max(40, w / 4));
  const int lineY = h * 56 / 100;
  const int lineGap = 25;

  tft.fillScreen(C(8, 10, 16));
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setFont(&fonts::efontKR_24);
  tft.setTextColor(C(255, 255, 255), C(8, 10, 16));
  tft.drawString(title, cx, titleY);
  tft.fillRoundRect(cx - ruleW / 2, titleY + 25, ruleW, 3, 1, C(0, 190, 255));
  tft.setFont(&fonts::efontKR_16);
  tft.setTextColor(C(170, 178, 192), C(8, 10, 16));
  if (line1) tft.drawString(line1, cx, lineY);
  if (line2) tft.drawString(line2, cx, lineY + lineGap);
  if (line3) tft.drawString(line3, cx, lineY + lineGap * 2);
}

static void showToast(const char* msg) {
  strlcpy(toastMsg, msg, sizeof(toastMsg));
  toastUntil = millis() + 2000;
}

template <typename GFX>
static void renderClockFace(GFX& g,
                            const char* dateStr,
                            const char* hh,
                            const char* mm,
                            const char* ampm,
                            const char* statusL,
                            const char* statusR,
                            bool timeValid,
                            bool colonOn,
                            int barW,
                            bool wifiOk,
                            bool toastOn) {
  const Theme& theme = THEMES[themeIndex % THEME_COUNT];
  const int w = screenW;
  const int h = screenH;
  const int cx = w / 2;
  const int marginX = max(16, w / 16);
  const int barFullW = max(80, w - marginX * 2);
  const int topY   = max(14, h * 9 / 100);
  const int timeY  = h * 48 / 100;
  const int barY   = h * 80 / 100;
  const int statusY = h - 16;

  drawThemedBackground(g, theme, w, h);

  // ── 상단 좌: 날짜 ──
  g.setFont(&fonts::efontKR_16);
  g.setTextDatum(textdatum_t::middle_left);
  g.setTextColor(TC(theme.muted));
  g.drawString(dateStr, marginX, topY);

  // ── 상단 우: 날씨(아이콘 + 기온), 그 아래 날씨 설명 ──
  {
    char tempStr[10];
    if (weatherValid) snprintf(tempStr, sizeof(tempStr), "%d", (int)lroundf(weatherTemp));
    else              strcpy(tempStr, "--");
    const int rightX = w - marginX;
    const int degR = 3;
    g.setFont(&fonts::efontKR_24);
    int tw = g.textWidth(tempStr);
    int tempRight = rightX - (degR * 2 + 2);
    g.setTextDatum(textdatum_t::middle_right);
    g.setTextColor(TC(theme.fg));
    g.drawString(tempStr, tempRight, topY);
    g.drawCircle(rightX - degR, topY - 8, degR, TC(theme.fg));   // 도(°) 기호
    drawWeatherIcon(g, tempRight - tw - 16, topY, weatherValid ? weatherCode : -1, theme);

    g.setFont(&fonts::efontKR_16);
    g.setTextDatum(textdatum_t::middle_right);
    g.setTextColor(TC(theme.muted));
    g.drawString(weatherValid ? weatherLabel(weatherCode) : "날씨 불러오는 중", rightX, topY + 22);
  }

  // ── 중앙: 시간 (콜론은 숨쉬듯 깜빡임) ──
  g.setFont(&fonts::Font8);
  int wH = g.textWidth(hh), wC = g.textWidth(":"), wM = g.textWidth(mm);
  int x0 = cx - (wH + wC + wM) / 2;
  g.setTextDatum(textdatum_t::middle_left);
  g.setTextColor(TC(theme.faint));
  g.drawString(hh, x0 + 2, timeY + 3);
  g.drawString(":", x0 + wH + 2, timeY + 3);
  g.drawString(mm, x0 + wH + wC + 2, timeY + 3);
  g.setTextColor(TC(theme.fg));
  g.drawString(hh, x0, timeY);
  g.setTextColor(colonOn ? TC(theme.fg) : TC(theme.faint));
  g.drawString(":", x0 + wH, timeY);
  g.setTextColor(TC(theme.fg));
  g.drawString(mm, x0 + wH + wC, timeY);

  // 오전/오후 (12시간제)
  if (use12h && timeValid) {
    g.setFont(&fonts::efontKR_16);
    g.setTextDatum(textdatum_t::middle_left);
    int ampmX = min(w - marginX - 40, x0 + wH + wC + wM + 12);
    g.setTextColor(TC(theme.accent));
    g.drawString(ampm, ampmX, timeY + 22);
  }

  // ── 초 진행 바 ──
  int safeBarW = constrain(barW, 0, barFullW);
  g.fillRoundRect(marginX, barY, barFullW, 6, 3, TC(theme.faint));
  if (safeBarW > 0) {
    if (safeBarW > 6) g.fillRoundRect(marginX, barY, safeBarW, 6, 3, TC(theme.accent));
    else              g.fillRect(marginX, barY, safeBarW, 6, TC(theme.accent));
    g.fillCircle(marginX + safeBarW, barY + 3, 3, TC(theme.accent2));
  }

  // ── 하단 상태 / 토스트 ──
  g.setFont(&fonts::efontKR_16);
  if (toastOn) {
    g.setTextDatum(textdatum_t::middle_center);
    int toastW = min(w - marginX * 2, g.textWidth(toastMsg) + 22);
    g.fillRoundRect(cx - toastW / 2, statusY - 13, toastW, 25, 8, TC(theme.panel));
    g.setTextColor(TC(theme.warn));
    g.drawString(toastMsg, cx, statusY);
  } else {
    drawWifiIcon(g, marginX + 4, statusY + 3, wifiOk ? TC(theme.accent) : TC(theme.faint));
    g.setTextDatum(textdatum_t::middle_left);
    g.setTextColor(TC(theme.muted));
    g.drawString(statusL, marginX + 22, statusY);
    g.setTextDatum(textdatum_t::middle_right);
    g.drawString(statusR, w - marginX, statusY);
  }
}

// ================= 메인 화면 =================
static void drawClockFrame() {
  const int w = screenW;
  const int h = screenH;
  const int marginX = max(16, w / 16);
  const int barFullW = max(80, w - marginX * 2);

  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  bool timeValid = (t.tm_year + 1900) >= 2020;

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  int subMs = tv.tv_usec / 1000;

  // --- 표시 내용 구성 ---
  char hh[4], mm[4], dateStr[64], statusL[40], statusR[40];
  const char* ampm = "";
  if (timeValid) {
    int hour = t.tm_hour;
    if (use12h) {
      ampm = (hour < 12) ? "오전" : "오후";
      hour = hour % 12; if (hour == 0) hour = 12;
      snprintf(hh, sizeof(hh), "%d", hour);
    } else {
      snprintf(hh, sizeof(hh), "%02d", hour);
    }
    snprintf(mm, sizeof(mm), "%02d", t.tm_min);
    snprintf(dateStr, sizeof(dateStr), "%d년 %d월 %d일 %s",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, WEEKDAY_KR[t.tm_wday]);
  } else {
    strcpy(hh, "--"); strcpy(mm, "--");
    strcpy(dateStr, "시간 동기화 대기 중");
  }

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  // 하단 좌: 날씨가 있으면 도시·습도, 없으면 WiFi 상태
  if (weatherValid) {
    if (weatherCity[0]) snprintf(statusL, sizeof(statusL), "%s · 습도 %d%%", weatherCity, weatherHum);
    else                snprintf(statusL, sizeof(statusL), "습도 %d%%", weatherHum);
  } else if (wifiOk) {
    snprintf(statusL, sizeof(statusL), "WiFi %ddBm", WiFi.RSSI());
  } else {
    snprintf(statusL, sizeof(statusL), "WiFi 재연결");
  }
  // 하단 우: 테마 · 밝기 모드
  snprintf(statusR, sizeof(statusR), "%s · %s", THEMES[themeIndex % THEME_COUNT].name, BRIGHT_NAME[brightMode]);

  bool colonOn = (subMs < 600);
  int  barW    = timeValid ? (int)((t.tm_sec * 1000 + subMs) * (long)barFullW / 60000L) : 0;
  bool toastOn = (millis() < toastUntil) && toastMsg[0];

  // 바뀐 게 없으면 다시 그리지 않음
  char sig[320];
  snprintf(sig, sizeof(sig), "%s:%s|%d|%d|%s|%s|%s|%d|%s|%d|%d|%d|%d|%d|%d|%d|%d",
           hh, mm, colonOn, barW, dateStr, statusL, statusR, wifiOk, toastOn ? toastMsg : "", (int)use12h, themeIndex, w, h,
           (int)weatherValid, (int)lroundf(weatherTemp), weatherHum, weatherCode);
  if (strcmp(sig, lastSig) == 0) return;
  strlcpy(lastSig, sig, sizeof(lastSig));

  if (canvasOk) {
    renderClockFace(canvas, dateStr, hh, mm, ampm, statusL, statusR,
                    timeValid, colonOn, barW, wifiOk, toastOn);
    canvas.pushSprite(0, 0);
  } else {
    tft.fillScreen(C(0, 0, 0));
    renderClockFace(tft, dateStr, hh, mm, ampm, statusL, statusR,
                    timeValid, colonOn, barW, wifiOk, toastOn);
  }
}

// ================= 밝기 =================
static void updateBrightness() {
  int target;
  if (brightMode == 0) {
    int raw = analogRead(PIN_LDR);
    ldrEma += (raw - ldrEma) * 0.1f;
    float dark = LDR_BRIGHT_IS_LOW
                   ? (ldrEma - LDR_RAW_BRIGHT) / (float)(LDR_RAW_DARK - LDR_RAW_BRIGHT)
                   : 1.0f - (ldrEma - LDR_RAW_BRIGHT) / (float)(LDR_RAW_DARK - LDR_RAW_BRIGHT);
    dark = constrain(dark, 0.0f, 1.0f);
    target = BRIGHTNESS_MIN + (int)((1.0f - dark) * (BRIGHTNESS_MAX - BRIGHTNESS_MIN));
  } else {
    target = BRIGHT_FIXED[brightMode];
  }
  if (abs(target - curBrightness) > 2) {
    curBrightness = target;
    tft.setBrightness(curBrightness);
  }
}

// ================= 터치 =================
static void handleTouch() {
  static bool     wasPressed = false;
  static uint32_t pressStart = 0;
  static int32_t  pressX     = 0;

  int32_t tx, ty;
  bool pressed = tft.getTouch(&tx, &ty);
  uint32_t nowMs = millis();

  if (pressed && !wasPressed) {          // 누르기 시작
    pressStart = nowMs;
    pressX     = tx;
  } else if (pressed && wasPressed) {    // 누르는 중
    uint32_t held = nowMs - pressStart;
    if (held >= 1200 && held < 5000) {
      char buf[64];
      snprintf(buf, sizeof(buf), "WiFi 초기화까지 %d초 유지", (int)((5000 - held) / 1000) + 1);
      showToast(buf);
    } else if (held >= 5000) {           // WiFi 설정 초기화
      drawScreen("WiFi 초기화", "저장된 WiFi 정보를 지우고", "다시 시작합니다...");
      wm.resetSettings();
      delay(1500);
      ESP.restart();
    }
  } else if (!pressed && wasPressed) {   // 손 뗌
    uint32_t held = nowMs - pressStart;
    if (held < 500) {                    // 짧은 탭
      if (pressX < screenW / 3) {
        use12h = !use12h;
        prefs.putBool("use12h", use12h);
        showToast(use12h ? "12시간제" : "24시간제");
      } else if (pressX < screenW * 2 / 3) {
        themeIndex = (themeIndex + 1) % THEME_COUNT;
        prefs.putUChar("theme", themeIndex);
        char buf[32];
        snprintf(buf, sizeof(buf), "테마: %s", THEMES[themeIndex].name);
        showToast(buf);
      } else {
        brightMode = (brightMode + 1) % 4;
        prefs.putUChar("bmode", brightMode);
        char buf[32];
        snprintf(buf, sizeof(buf), "밝기: %s", BRIGHT_NAME[brightMode]);
        showToast(buf);
      }
    } else {
      toastUntil = 0;                    // 길게 눌렀다 취소한 경우 토스트 제거
    }
  }
  wasPressed = pressed;
}

// ================= 설정 =================
static const char* wifiStatusHint(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED:     return "연결됨";
    case WL_NO_SSID_AVAIL: return "SSID 없음: 2.4GHz/채널 확인";
    case WL_CONNECT_FAILED:return "연결 실패: 비밀번호/WPA2 확인";
    case WL_CONNECTION_LOST:return "연결 끊김: 신호 확인";
    case WL_DISCONNECTED:  return "연결 안 됨: 설정 필요";
    case WL_IDLE_STATUS:   return "연결 대기 중";
    default:               return "연결 실패";
  }
}

static void onPortalStart(WiFiManager* w) {
  drawScreen("WiFi 설정 필요",
             "휴대폰 WiFi에서 'CYD-Clock' 접속",
             "2.4GHz / WPA2 WiFi를 선택",
             "5GHz, WPA3-only는 연결 불가");
}

static bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);

  wm.setDebugOutput(true);
  wm.setHostname(HOSTNAME);
  wm.setCountry("KR");
  wm.setCleanConnect(true);
  wm.setConnectRetries(3);
  wm.setConnectTimeout(35);
  wm.setSaveConnectTimeout(35);
  wm.setWiFiAutoReconnect(true);
  wm.setMinimumSignalQuality(0);
  wm.setRemoveDuplicateAPs(false);
  wm.setShowPassword(true);
  wm.setScanDispPerc(true);
  wm.setAPCallback(onPortalStart);
  wm.setConfigPortalTimeout(0);  // 연결될 때까지 설정 포털 유지

  if (!wifiHintAdded) {
    wm.addParameter(&wifiHint);
    wifiHintAdded = true;
  }

  drawScreen("CYD 데스크 시계", "WiFi 연결 중...", "2.4GHz 채널 1-13 지원");
  if (wm.autoConnect(AP_NAME)) {
    String ssidLine = WiFi.SSID();
    String ipLine = WiFi.localIP().toString();
    drawScreen("WiFi 연결됨", ssidLine.c_str(), ipLine.c_str());
    delay(900);
    return true;
  }

  wl_status_t status = WiFi.status();
  drawScreen("WiFi 연결 실패",
             wifiStatusHint(status),
             "2.4GHz / WPA2 / 비밀번호 확인",
             "설정 화면을 다시 열어 주세요");
  delay(3000);
  return false;
}

static void maintainWiFi() {
  static uint32_t lastCheck = 0;
  static uint32_t lastReconnect = 0;
  static bool wasConnected = true;

  uint32_t nowMs = millis();
  if (nowMs - lastCheck < 1000) return;
  lastCheck = nowMs;

  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected) {
    if (!wasConnected) {
      showToast("WiFi 재연결됨");
      configTzTime(TZ_INFO, NTP_1, NTP_2, NTP_3);
    }
    wasConnected = true;
    return;
  }

  if (wasConnected) showToast("WiFi 끊김");
  wasConnected = false;

  if (lastReconnect == 0 || nowMs - lastReconnect >= 10000) {
    lastReconnect = nowMs;
    WiFi.reconnect();
  }
}

// ================= 날씨 가져오기 =================
// JSON에서 "key":숫자 형태의 값을 뽑아냅니다 (경량 파서, ArduinoJson 의존성 없음).
static bool jsonNumber(const String& body, const char* key, float& out) {
  String pat = String("\"") + key + "\":";
  int i = body.indexOf(pat);
  if (i < 0) return false;
  i += pat.length();
  while (i < (int)body.length() && body[i] == ' ') i++;
  int j = i;
  while (j < (int)body.length()) {
    char c = body[j];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') j++;
    else break;
  }
  if (j == i) return false;
  out = body.substring(i, j).toFloat();
  return true;
}

// JSON에서 "key":"문자열" 값을 뽑아냅니다.
static bool jsonString(const String& body, const char* key, char* out, size_t n) {
  String pat = String("\"") + key + "\":\"";
  int i = body.indexOf(pat);
  if (i < 0) return false;
  i += pat.length();
  int j = body.indexOf('"', i);
  if (j < 0) return false;
  strlcpy(out, body.substring(i, j).c_str(), n);
  return true;
}

// 위치 결정: 수동 좌표가 있으면 사용, 없으면 IP 기반(ip-api.com, 무료·HTTP)으로 1회 조회.
static bool resolveLocation() {
  if (geoResolved) return true;
  if (MANUAL_LAT != 0.0f || MANUAL_LON != 0.0f) {
    geoLat = MANUAL_LAT; geoLon = MANUAL_LON; geoResolved = true;
    return true;
  }
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin("http://ip-api.com/json/?fields=status,lat,lon,city")) return false;
  bool ok = false;
  if (http.GET() == 200) {
    String body = http.getString();
    float la, lo;
    if (jsonNumber(body, "lat", la) && jsonNumber(body, "lon", lo)) {
      geoLat = la; geoLon = lo;
      jsonString(body, "city", weatherCity, sizeof(weatherCity));
      geoResolved = true;
      ok = true;
    }
  }
  http.end();
  return ok;
}

// Open-Meteo(무료·API 키 불필요)에서 현재 기온/습도/날씨코드를 가져옵니다.
static void fetchWeather() {
  if (!resolveLocation()) return;
  WiFiClientSecure client;
  client.setInsecure();                  // 취미용 시계: 인증서 검증 생략
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  char url[220];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code&timezone=auto",
           geoLat, geoLon);
  if (!http.begin(client, url)) return;
  if (http.GET() == 200) {
    String body = http.getString();
    int ci = body.indexOf("\"current\"");
    String seg = (ci >= 0) ? body.substring(ci) : body;
    float temp, hum, wc;
    if (jsonNumber(seg, "temperature_2m", temp) &&
        jsonNumber(seg, "relative_humidity_2m", hum) &&
        jsonNumber(seg, "weather_code", wc)) {
      weatherTemp  = temp;
      weatherHum   = (int)lroundf(hum);
      weatherCode  = (int)wc;
      weatherValid = true;
    }
  }
  http.end();
}

// 백그라운드 태스크(코어 0): 시계 렌더링(코어 1)을 막지 않고 주기적으로 갱신.
static void weatherTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(2500));
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) fetchWeather();
    vTaskDelay(pdMS_TO_TICKS(weatherValid ? 15UL * 60UL * 1000UL : 60UL * 1000UL));
  }
}

void setup() {
  Serial.begin(115200);

  // 뒷면 RGB LED 끄기 (active-low)
  pinMode(PIN_LED_R, OUTPUT); digitalWrite(PIN_LED_R, HIGH);
  pinMode(PIN_LED_G, OUTPUT); digitalWrite(PIN_LED_G, HIGH);
  pinMode(PIN_LED_B, OUTPUT); digitalWrite(PIN_LED_B, HIGH);

  analogSetPinAttenuation(PIN_LDR, ADC_11db);

  tft.init();
  setLandscapeRotation();
  tft.invertDisplay(PANEL_INVERT);
  tft.setBrightness(180);

  canvas.setColorDepth(8);
  canvasOk = canvas.createSprite(screenW, screenH) != nullptr;
  if (!canvasOk) {
    Serial.println("Failed to allocate display canvas");
  }

  prefs.begin("clock", false);
  use12h     = prefs.getBool("use12h", false);
  brightMode = prefs.getUChar("bmode", 0) % 4;
  themeIndex = prefs.getUChar("theme", 0) % THEME_COUNT;

  drawScreen("CYD 데스크 시계", "WiFi 연결 중...");

  while (!connectWiFi()) {
    delay(1000);
  }

  drawScreen("CYD 데스크 시계", "시간 동기화 중...");
  configTzTime(TZ_INFO, NTP_1, NTP_2, NTP_3);
  uint32_t start = millis();
  while (millis() - start < 15000) {     // 최대 15초 대기 (실패해도 백그라운드 동기화 계속됨)
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 >= 2020) break;
    delay(200);
  }

  // 날씨 갱신을 백그라운드 코어에서 실행 (시계 렌더링이 끊기지 않도록)
  xTaskCreatePinnedToCore(weatherTask, "weather", 8192, nullptr, 1, nullptr, 0);

  lastSig[0] = '\0';
}

void loop() {
  handleTouch();
  maintainWiFi();

  static uint32_t lastBright = 0;
  if (millis() - lastBright >= 100) {
    lastBright = millis();
    updateBrightness();
  }

  drawClockFrame();
  delay(20);
}
