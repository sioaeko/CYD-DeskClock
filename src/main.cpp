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
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "timefont.h"   // 대형 안티에일리어스 숫자 폰트 (시계용)

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
static WebServer server(80);
static WiFiManagerParameter wifiHint(
  "<p style='font-size:13px;color:#666'>ESP32는 2.4GHz WiFi만 지원합니다. "
  "공유기가 WPA3-only이면 WPA2/WPA3 혼합 또는 WPA2로 바꿔 주세요.</p>");
static bool wifiHintAdded = false;

// ================= 상태 =================
static bool    use12h     = false;
static uint8_t brightMode = 0;              // 0=자동 1=최대 2=중간 3=최소
static uint8_t themeIndex = 0;
static uint8_t nightMode  = 0;              // 0=끔 1=자동(밤) 2=항상 켬
static const uint8_t BRIGHT_FIXED[4] = { 0, 255, 110, 25 };
static const char*   BRIGHT_NAME[4]  = { "자동", "최대", "중간", "최소" };
static const int NIGHT_BRIGHTNESS = 6;      // 야간 모드 백라이트 (아주 어둡게)

struct Theme {
  const char* name;
  uint8_t bgTop[3], bgBottom[3], panel[3], fg[3], muted[3], faint[3], accent[3], accent2[3], warn[3];
};

static const Theme THEMES[] = {
  { "네온", { 8, 12, 24 }, { 0, 0, 0 }, { 13, 17, 28 }, { 245, 248, 252 }, { 150, 160, 178 }, { 84, 92, 108 }, { 0, 190, 255 }, { 95, 235, 185 }, { 255, 214, 90 } },
  { "앰버", { 26, 13, 7 }, { 2, 1, 0 }, { 24, 14, 8 }, { 255, 244, 222 }, { 188, 160, 128 }, { 104, 78, 54 }, { 255, 170, 58 }, { 255, 91, 74 }, { 255, 222, 120 } },
  { "포레스트", { 5, 24, 22 }, { 0, 4, 3 }, { 8, 25, 22 }, { 232, 250, 242 }, { 137, 174, 160 }, { 72, 104, 92 }, { 74, 222, 128 }, { 56, 189, 248 }, { 250, 204, 21 } },
  { "모노", { 15, 16, 20 }, { 0, 0, 0 }, { 18, 19, 24 }, { 250, 250, 250 }, { 160, 164, 172 }, { 86, 90, 102 }, { 220, 224, 232 }, { 118, 128, 142 }, { 255, 214, 90 } },
  { "오션", { 6, 16, 30 }, { 0, 2, 6 }, { 10, 20, 34 }, { 224, 242, 255 }, { 138, 164, 192 }, { 66, 90, 116 }, { 56, 160, 255 }, { 60, 220, 220 }, { 255, 209, 102 } },
  { "선셋", { 28, 12, 20 }, { 4, 1, 4 }, { 28, 14, 22 }, { 255, 240, 238 }, { 196, 150, 158 }, { 112, 74, 84 }, { 255, 110, 130 }, { 255, 170, 90 }, { 255, 220, 140 } },
  { "자수정", { 18, 12, 30 }, { 2, 0, 6 }, { 20, 15, 32 }, { 240, 236, 255 }, { 168, 156, 200 }, { 92, 80, 124 }, { 170, 130, 255 }, { 120, 200, 255 }, { 255, 214, 120 } },
  { "로즈", { 26, 10, 16 }, { 4, 0, 3 }, { 28, 12, 18 }, { 255, 238, 244 }, { 198, 150, 166 }, { 112, 72, 88 }, { 255, 90, 140 }, { 255, 150, 180 }, { 255, 210, 130 } },
};
static const uint8_t THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);

// 야간 모드 전용 테마: 눈부심 없는 어두운 적색(암순응에 유리).
static const Theme NIGHT_THEME =
  { "야간", { 8, 1, 0 }, { 0, 0, 0 }, { 14, 3, 2 }, { 200, 66, 42 }, { 128, 44, 30 }, { 58, 20, 14 }, { 220, 74, 48 }, { 150, 52, 36 }, { 210, 84, 44 } };

// 야간 모드가 지금 활성인가? (1=항상, 2=밤 시간대 22시~07시)
static bool nightActive() {
  if (nightMode == 2) return true;
  if (nightMode == 1) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2020) return false;   // 아직 시간 동기화 전
    int hh = t.tm_hour;
    return (hh >= 22 || hh < 7);
  }
  return false;
}

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
static volatile bool  weatherRefresh = false;   // 위치 변경 시 즉시 갱신 요청
static bool           locManual = false;         // true=사용자 지정 위치, false=IP 자동

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
  const Theme& theme = nightActive() ? NIGHT_THEME : THEMES[themeIndex % THEME_COUNT];
  const int w = screenW;
  const int h = screenH;
  const int cx = w / 2;
  const int marginX = max(16, w / 16);
  const int barFullW = max(80, w - marginX * 2);
  const int topY   = max(14, h * 7 / 100);
  const int timeY  = h * 45 / 100;
  const int barY   = h * 75 / 100;
  const int statusY = h - 15;

  drawThemedBackground(g, theme, w, h);

  // ── 상단 좌: 날짜 ──
  g.setFont(&fonts::efontKR_16);
  g.setTextDatum(textdatum_t::middle_left);
  g.setTextColor(TC(theme.muted));
  g.drawString(dateStr, marginX, topY);

  // ── 상단 우: 날씨(아이콘 + 기온) ── (설명/습도는 하단 우측에)
  {
    char tempStr[10];
    if (weatherValid) snprintf(tempStr, sizeof(tempStr), "%d", (int)lroundf(weatherTemp));
    else              strcpy(tempStr, "--");
    const int rightX = w - marginX;
    const int degR = 3;
    g.setFont(&fonts::FreeSansBold18pt7b);
    int tw = g.textWidth(tempStr);
    int tempRight = rightX - (degR * 2 + 2);
    g.setTextDatum(textdatum_t::middle_right);
    g.setTextColor(TC(theme.fg));
    g.drawString(tempStr, tempRight, topY);
    g.drawCircle(rightX - degR, topY - 9, degR, TC(theme.fg));   // 도(°) 기호
    drawWeatherIcon(g, tempRight - tw - 16, topY, weatherValid ? weatherCode : -1, theme);

    // 날씨 설명 (기온 아래, 우측 정렬)
    g.setFont(&fonts::efontKR_16);
    g.setTextDatum(textdatum_t::middle_right);
    g.setTextColor(TC(theme.muted));
    g.drawString(weatherValid ? weatherLabel(weatherCode) : "불러오는 중", rightX, topY + 24);
  }

  // ── 중앙: 시간 (대형 안티에일리어스 숫자, 콜론은 숨쉬듯 깜빡임) ──
  g.loadFont(timefont_vlw);
  int wH = g.textWidth(hh), wC = g.textWidth(":"), wM = g.textWidth(mm);
  int timeW = wH + wC + wM;
  // 12시간제면 오른쪽 '오후' 라벨 폭(약 44px)까지 감안해 전체를 중앙 정렬
  int ampmSpace = (use12h && timeValid) ? 46 : 0;
  int x0 = cx - (timeW + ampmSpace) / 2;
  g.setTextDatum(textdatum_t::middle_left);
  g.setTextColor(TC(theme.fg));
  g.drawString(hh, x0, timeY);
  g.setTextColor(colonOn ? TC(theme.fg) : TC(theme.faint));
  g.drawString(":", x0 + wH, timeY);
  g.setTextColor(TC(theme.fg));
  g.drawString(mm, x0 + wH + wC, timeY);
  g.unloadFont();

  // 오전/오후 (12시간제) — 시간 오른쪽, 아래쪽 정렬
  if (use12h && timeValid) {
    g.setFont(&fonts::efontKR_16);
    g.setTextDatum(textdatum_t::baseline_left);
    g.setTextColor(TC(theme.accent));
    g.drawString(ampm, x0 + timeW + 12, timeY + 22);
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
    snprintf(dateStr, sizeof(dateStr), "%d월 %d일 %s",
             t.tm_mon + 1, t.tm_mday, WEEKDAY_KR[t.tm_wday]);   // 연도 생략 — 여백 확보
  } else {
    strcpy(hh, "--"); strcpy(mm, "--");
    strcpy(dateStr, "시간 동기화 대기 중");
  }

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  // 하단 좌: 도시(없으면 WiFi 상태)
  if (weatherValid && weatherCity[0]) {
    snprintf(statusL, sizeof(statusL), "%s", weatherCity);
  } else if (wifiOk) {
    snprintf(statusL, sizeof(statusL), "WiFi %ddBm", WiFi.RSSI());
  } else {
    snprintf(statusL, sizeof(statusL), "WiFi 재연결");
  }
  // 하단 우: 습도 (없으면 빈칸)
  if (weatherValid) snprintf(statusR, sizeof(statusR), "습도 %d%%", weatherHum);
  else              statusR[0] = '\0';

  bool colonOn = (subMs < 600);
  int  barW    = timeValid ? (int)((t.tm_sec * 1000 + subMs) * (long)barFullW / 60000L) : 0;
  bool toastOn = (millis() < toastUntil) && toastMsg[0];

  // 바뀐 게 없으면 다시 그리지 않음
  char sig[320];
  snprintf(sig, sizeof(sig), "%s:%s|%d|%d|%s|%s|%s|%d|%s|%d|%d|%d|%d|%d|%d|%d|%d|%d",
           hh, mm, colonOn, barW, dateStr, statusL, statusR, wifiOk, toastOn ? toastMsg : "", (int)use12h, themeIndex, w, h,
           (int)weatherValid, (int)lroundf(weatherTemp), weatherHum, weatherCode, (int)nightActive());
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
  if (nightActive()) {
    target = NIGHT_BRIGHTNESS;              // 야간: 밝기 모드와 무관하게 아주 어둡게
  } else if (brightMode == 0) {
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

// ================= 설정 적용 (터치 / 웹 공용) =================
static void applyTheme(uint8_t i, bool toast) {
  themeIndex = (uint8_t)(i % THEME_COUNT);
  prefs.putUChar("theme", themeIndex);
  if (toast) { char b[32]; snprintf(b, sizeof(b), "테마: %s", THEMES[themeIndex].name); showToast(b); }
}
static void applyBright(uint8_t m, bool toast) {
  brightMode = (uint8_t)(m % 4);
  prefs.putUChar("bmode", brightMode);
  curBrightness = -1;  // 다음 루프에서 즉시 다시 계산
  if (toast) { char b[32]; snprintf(b, sizeof(b), "밝기: %s", BRIGHT_NAME[brightMode]); showToast(b); }
}
static void applyUse12h(bool v, bool toast) {
  use12h = v;
  prefs.putBool("use12h", use12h);
  if (toast) showToast(use12h ? "12시간제" : "24시간제");
}
static void applyNight(uint8_t m, bool toast) {
  nightMode = (uint8_t)(m % 3);
  prefs.putUChar("night", nightMode);
  curBrightness = -1;  // 밝기 즉시 재계산
  if (toast) {
    static const char* n[3] = { "야간 모드: 끔", "야간 모드: 자동(밤)", "야간 모드: 켬" };
    showToast(n[nightMode]);
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
        applyUse12h(!use12h, true);
      } else if (pressX < screenW * 2 / 3) {
        applyTheme(themeIndex + 1, true);
      } else {
        applyBright(brightMode + 1, true);
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
    String ipLine = "http://" + WiFi.localIP().toString();
    drawScreen("WiFi 연결됨", ssidLine.c_str(), "설정: http://cyd-clock.local", ipLine.c_str());
    delay(2500);
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

// URL 쿼리용 퍼센트 인코딩 (한글/공백 포함 도시명 검색용).
static String urlEncode(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%'; out += hex[c >> 4]; out += hex[c & 0x0F];
    }
  }
  return out;
}

static void persistLocation() {
  prefs.putBool("locman", locManual);
  if (locManual) {
    prefs.putFloat("lat", geoLat);
    prefs.putFloat("lon", geoLon);
    prefs.putString("city", weatherCity);
  }
}

// 도시 이름 → 좌표 (Open-Meteo 지오코딩, 무료·키 불필요). 성공 시 수동 위치로 저장.
static bool geocodePlace(const String& name) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  String url = "https://geocoding-api.open-meteo.com/v1/search?count=1&language=ko&name=" + urlEncode(name);
  if (!http.begin(client, url)) return false;
  bool ok = false;
  if (http.GET() == 200) {
    String body = http.getString();
    int ri = body.indexOf("\"results\"");
    if (ri >= 0) {
      String seg = body.substring(ri);
      float la, lo;
      if (jsonNumber(seg, "latitude", la) && jsonNumber(seg, "longitude", lo)) {
        geoLat = la; geoLon = lo;
        if (!jsonString(seg, "name", weatherCity, sizeof(weatherCity)))
          strlcpy(weatherCity, name.c_str(), sizeof(weatherCity));
        geoResolved = true;
        locManual = true;
        persistLocation();
        ok = true;
      }
    }
  }
  http.end();
  return ok;
}

// 자동(IP) 위치로 복귀.
static void resetToAutoLocation() {
  locManual = false;
  geoResolved = false;
  weatherCity[0] = '\0';
  prefs.putBool("locman", false);
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
// 평소엔 주기적으로, 지역이 바뀌면(weatherRefresh) 즉시 갱신.
static void weatherTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(2500));
  uint32_t lastFetch = 0;
  for (;;) {
    uint32_t nowMs = millis();
    uint32_t interval = weatherValid ? 15UL * 60UL * 1000UL : 60UL * 1000UL;
    bool due = (lastFetch == 0) || (nowMs - lastFetch >= interval);
    if (WiFi.status() == WL_CONNECTED && (due || weatherRefresh)) {
      weatherRefresh = false;
      fetchWeather();
      lastFetch = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(400));
  }
}

// ================= 웹 설정 페이지 =================
// 같은 WiFi에서 http://cyd-clock.local (또는 기기 IP) 접속 시 테마/밝기/시간표시 변경.
static const char CONTROL_PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=ko><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>CYD 시계 설정</title>
<style>
:root{--bg:#0b0d12;--card:#151a23;--edge:#232a36;--text:#e9edf3;--muted:#95a0b2;--accent:#38d0ff}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Malgun Gothic",sans-serif;line-height:1.5;padding:26px 16px 48px}
.wrap{max-width:520px;margin:0 auto}
h1{font-size:22px;font-weight:800;text-align:center}
.sub{text-align:center;color:var(--muted);font-size:13px;margin:4px 0 22px}
.sec{margin-top:26px}
.sec>h2{font-size:12px;letter-spacing:.14em;text-transform:uppercase;color:var(--accent);margin-bottom:12px}
.themes{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}
.tc{border:2px solid var(--edge);border-radius:14px;overflow:hidden;cursor:pointer;background:var(--card);transition:border-color .15s,transform .08s}
.tc:active{transform:scale(.98)}
.tc.on{border-color:var(--accent)}
.tc .pv{height:58px;display:flex;align-items:center;justify-content:center;position:relative;font-weight:800;font-size:20px;letter-spacing:1px;font-variant-numeric:tabular-nums}
.tc .pv .dot{position:absolute;right:10px;bottom:8px;width:12px;height:12px;border-radius:50%}
.tc .nm{padding:9px 12px;font-size:14px;font-weight:600;display:flex;justify-content:space-between;align-items:center}
.tc .ck{color:var(--accent);opacity:0}
.tc.on .ck{opacity:1}
.seg{display:flex;gap:8px;flex-wrap:wrap}
.seg button{flex:1;min-width:66px;padding:12px;border:1px solid var(--edge);background:var(--card);color:var(--muted);border-radius:12px;font-size:15px;font-weight:600;cursor:pointer;font-family:inherit}
.seg button.on{border-color:var(--accent);color:var(--text);background:rgba(56,208,255,.12)}
.loc{display:flex;gap:8px}
.loc input{flex:1;min-width:0;padding:12px 14px;border:1px solid var(--edge);background:#0d1219;color:var(--text);border-radius:12px;font-size:15px;font-family:inherit}
.loc input::placeholder{color:#5f6a7a}
.loc input:focus{outline:none;border-color:var(--accent)}
.loc button,.locrow button{padding:12px 18px;border:none;background:var(--accent);color:#04141c;border-radius:12px;font-size:15px;font-weight:700;cursor:pointer;font-family:inherit}
.locrow{display:flex;justify-content:space-between;align-items:center;margin-top:10px}
.locrow .cur{color:var(--muted);font-size:14px}
.locrow button.ghost{background:transparent;border:1px solid var(--edge);color:var(--muted);font-weight:600;padding:8px 14px}
.hint{color:#6b7484;font-size:12px;margin-top:8px;line-height:1.45}
.foot{text-align:center;color:#5a6270;font-size:12px;margin-top:30px}
</style></head><body><div class=wrap>
<h1>CYD 데스크 시계</h1><div class=sub>같은 WiFi에서 실시간으로 조작하세요</div>
<div class=sec><h2>테마</h2><div class=themes id=themes></div></div>
<div class=sec><h2>밝기</h2><div class=seg id=bright></div></div>
<div class=sec><h2>시간 표시</h2><div class=seg id=h12>
<button data-h=0>24시간제</button><button data-h=1>12시간제</button></div></div>
<div class=sec><h2>야간 모드</h2><div class=seg id=night>
<button data-n=0>끔</button><button data-n=1>자동(밤)</button><button data-n=2>켬</button></div>
<div class=hint>자동은 밤 22시~오전 7시에 화면을 아주 어둡게, 눈부심 없는 붉은 톤으로 바꿉니다.</div></div>
<div class=sec><h2>지역 (날씨)</h2>
<div class=loc><input id=place placeholder="도시 이름 (예: 서울, 부산, Tokyo)" autocomplete=off><button id=applyloc>적용</button></div>
<div class=locrow><span class=cur id=curloc>—</span><button class=ghost id=autoloc>자동(IP)</button></div>
</div>
<div class=foot>변경은 즉시 시계에 적용되고 재부팅 후에도 유지됩니다</div>
</div><script>
let busy=0;
async function load(){if(busy)return;const s=await(await fetch('/api/state')).json();render(s)}
async function set(q){busy=1;const s=await(await fetch('/api/set?'+q)).json();render(s);busy=0}
function render(S){
 const T=document.getElementById('themes');T.innerHTML='';
 S.themes.forEach((t,i)=>{const d=document.createElement('div');d.className='tc'+(i==S.theme?' on':'');
  d.innerHTML=`<div class=pv style="background:${t.bg};color:${t.fg}">12:34<span class=dot style="background:${t.ac}"></span></div><div class=nm><span>${t.n}</span><span class=ck>&#10003;</span></div>`;
  d.onclick=()=>set('theme='+i);T.appendChild(d)});
 const B=document.getElementById('bright');B.innerHTML='';
 S.brightNames.forEach((n,i)=>{const b=document.createElement('button');b.textContent=n;if(i==S.bright)b.className='on';b.onclick=()=>set('bright='+i);B.appendChild(b)});
 document.querySelectorAll('#h12 button').forEach(b=>{b.classList.toggle('on',(+b.dataset.h)==S.h12);b.onclick=()=>set('h12='+b.dataset.h)});
 document.querySelectorAll('#night button').forEach(b=>{b.classList.toggle('on',(+b.dataset.n)==S.night);b.onclick=()=>set('night='+b.dataset.n)});
 document.getElementById('curloc').textContent=S.city?('현재: '+S.city+(S.auto?' · 자동':' · 지정')):(S.auto?'자동(IP)으로 감지 중':'—');
}
function applyPlace(){const v=document.getElementById('place').value.trim();if(v){document.getElementById('curloc').textContent='검색 중…';set('place='+encodeURIComponent(v));document.getElementById('place').value=''}}
document.getElementById('applyloc').onclick=applyPlace;
document.getElementById('place').addEventListener('keydown',e=>{if(e.key==='Enter')applyPlace()});
document.getElementById('autoloc').onclick=()=>set('autoloc=1');
load();setInterval(load,4000);
</script></body></html>)HTML";

static void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", CONTROL_PAGE);
}

static void sendState() {
  String j = "{\"theme\":" + String(themeIndex) +
             ",\"bright\":" + String(brightMode) +
             ",\"h12\":" + String(use12h ? 1 : 0) + ",\"brightNames\":[";
  for (int i = 0; i < 4; i++) { if (i) j += ","; j += "\"" + String(BRIGHT_NAME[i]) + "\""; }
  j += "],\"themes\":[";
  for (int i = 0; i < THEME_COUNT; i++) {
    char bg[8], fg[8], ac[8];
    snprintf(bg, sizeof(bg), "#%02x%02x%02x", THEMES[i].bgTop[0], THEMES[i].bgTop[1], THEMES[i].bgTop[2]);
    snprintf(fg, sizeof(fg), "#%02x%02x%02x", THEMES[i].fg[0], THEMES[i].fg[1], THEMES[i].fg[2]);
    snprintf(ac, sizeof(ac), "#%02x%02x%02x", THEMES[i].accent[0], THEMES[i].accent[1], THEMES[i].accent[2]);
    if (i) j += ",";
    j += "{\"n\":\"" + String(THEMES[i].name) + "\",\"bg\":\"" + bg + "\",\"fg\":\"" + fg + "\",\"ac\":\"" + ac + "\"}";
  }
  j += "],\"night\":" + String(nightMode) +
       ",\"city\":\"" + String(weatherCity) + "\",\"auto\":" + String(locManual ? 0 : 1) + "}";
  server.send(200, "application/json; charset=utf-8", j);
}

static void handleSet() {
  if (server.hasArg("theme"))  applyTheme((uint8_t)server.arg("theme").toInt(), true);
  if (server.hasArg("bright")) applyBright((uint8_t)server.arg("bright").toInt(), true);
  if (server.hasArg("h12"))    applyUse12h(server.arg("h12").toInt() != 0, true);
  if (server.hasArg("night"))  applyNight((uint8_t)server.arg("night").toInt(), true);
  if (server.hasArg("place")) {
    String p = server.arg("place"); p.trim();
    if (p.length()) {
      if (geocodePlace(p)) {
        weatherValid = false; weatherRefresh = true;
        char b[48]; snprintf(b, sizeof(b), "지역: %s", weatherCity); showToast(b);
      } else {
        showToast("지역을 찾지 못했습니다");
      }
    }
  }
  if (server.hasArg("autoloc")) {
    resetToAutoLocation();
    weatherValid = false; weatherRefresh = true;
    showToast("지역: 자동(IP)");
  }
  sendState();
}

static void startWebServer() {
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  server.on("/", handleRoot);
  server.on("/api/state", sendState);
  server.on("/api/set", handleSet);
  server.onNotFound(handleRoot);
  server.begin();
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
  nightMode  = prefs.getUChar("night", 0) % 3;

  // 저장된 지역(수동 위치) 복원
  locManual = prefs.getBool("locman", false);
  if (locManual) {
    geoLat = prefs.getFloat("lat", 0.0f);
    geoLon = prefs.getFloat("lon", 0.0f);
    String c = prefs.getString("city", "");
    strlcpy(weatherCity, c.c_str(), sizeof(weatherCity));
    geoResolved = (geoLat != 0.0f || geoLon != 0.0f);
  }

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

  // 웹 설정 페이지 (http://cyd-clock.local)
  startWebServer();

  lastSig[0] = '\0';
}

void loop() {
  server.handleClient();
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
