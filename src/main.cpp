/*
 * CYD 데스크 시계 — ESP32-2432S028R (Cheap Yellow Display, 2.8" ILI9341 + XPT2046)
 *
 * 기능
 *  - WiFiManager 캡티브 포털로 WiFi 설정 (핫스팟: CYD-Clock)
 *  - NTP 시간 동기화 (한국 표준시), 이후 자동 재동기화
 *  - 큰 시계 + 한글 날짜/요일 + 초 진행 바
 *  - 조도센서(LDR) 자동 밝기 조절
 *  - 터치: 왼쪽 탭 = 12/24시간제 전환, 오른쪽 탭 = 밝기 모드 전환,
 *          5초 길게 누르기 = WiFi 설정 초기화
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ================= 사용자 설정 =================
static const char* TZ_INFO = "KST-9";            // 한국 표준시 (변경: https://gist.github.com/alwynallan/24d96091655391107939 참고)
static const char* NTP_1   = "kr.pool.ntp.org";
static const char* NTP_2   = "time.google.com";
static const char* NTP_3   = "pool.ntp.org";
static const char* AP_NAME = "CYD-Clock";        // 최초 WiFi 설정용 핫스팟 이름
static const uint8_t CLOCK_ROTATION = 6;         // TPM408/CYD v2 가로 방향. 뒤집히면 4로 변경.

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

// ================= 상태 =================
static bool    use12h     = false;
static uint8_t brightMode = 0;              // 0=자동 1=최대 2=중간 3=최소
static const uint8_t BRIGHT_FIXED[4] = { 0, 255, 110, 25 };
static const char*   BRIGHT_NAME[4]  = { "자동", "최대", "중간", "최소" };

static float    ldrEma        = 2000.0f;
static int      curBrightness = -1;
static char     toastMsg[64]  = "";
static uint32_t toastUntil    = 0;
static char     lastSig[192]  = "";

static const char* WEEKDAY_KR[7] = { "일요일", "월요일", "화요일", "수요일", "목요일", "금요일", "토요일" };

// 색상 (RGB888)
static inline uint32_t C(uint8_t r, uint8_t g, uint8_t b) { return lgfx::color888(r, g, b); }

// ================= 그리기 유틸 =================
template <typename GFX>
static void drawWifiIcon(GFX& g, int x, int y, uint32_t col) {
  g.fillCircle(x, y, 2, col);
  g.fillArc(x, y, 6, 4, 225, 315, col);
  g.fillArc(x, y, 11, 9, 225, 315, col);
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
  const int w = screenW;
  const int h = screenH;
  const int cx = w / 2;
  const int marginX = max(16, w / 16);
  const int barFullW = max(80, w - marginX * 2);
  const int dateY = max(10, h * 7 / 100);
  const int timeY = h * 47 / 100;
  const int barY = h * 71 / 100;
  const int statusY = h - 18;

  // 날짜 (상단)
  g.setFont(&fonts::efontKR_24);
  g.setTextDatum(textdatum_t::top_center);
  g.setTextColor(C(158, 168, 184));
  g.drawString(dateStr, cx, dateY);

  // 시간 (중앙, 콜론은 숨쉬듯 깜빡임)
  g.setFont(&fonts::Font8);
  int wH = g.textWidth(hh), wC = g.textWidth(":"), wM = g.textWidth(mm);
  int x0 = cx - (wH + wC + wM) / 2;
  g.setTextDatum(textdatum_t::middle_left);
  g.setTextColor(C(245, 247, 250));
  g.drawString(hh, x0, timeY);
  g.setTextColor(colonOn ? C(245, 247, 250) : C(45, 48, 56));
  g.drawString(":", x0 + wH, timeY);
  g.setTextColor(C(245, 247, 250));
  g.drawString(mm, x0 + wH + wC, timeY);

  // 오전/오후 (12시간제)
  if (use12h && timeValid) {
    g.setFont(&fonts::efontKR_16);
    g.setTextDatum(textdatum_t::middle_left);
    g.setTextColor(C(0, 190, 255));
    g.drawString(ampm, x0 + wH + wC + wM + 10, timeY + 20);
  }

  // 초 진행 바
  g.fillRoundRect(marginX, barY, barFullW, 6, 3, C(30, 33, 40));
  if (barW > 6) g.fillRoundRect(marginX, barY, barW, 6, 3, C(0, 190, 255));

  // 하단 상태줄 / 토스트
  g.setFont(&fonts::efontKR_16);
  if (toastOn) {
    g.setTextDatum(textdatum_t::middle_center);
    g.setTextColor(C(255, 214, 90));
    g.drawString(toastMsg, cx, statusY);
  } else {
    drawWifiIcon(g, marginX + 4, statusY + 4, wifiOk ? C(0, 190, 255) : C(70, 74, 84));
    g.setTextDatum(textdatum_t::middle_left);
    g.setTextColor(C(110, 118, 132));
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
  char hh[4], mm[4], dateStr[64], statusL[48], statusR[24];
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
  if (wifiOk) snprintf(statusL, sizeof(statusL), "%s", WiFi.localIP().toString().c_str());
  else        snprintf(statusL, sizeof(statusL), "WiFi 재연결 중...");
  snprintf(statusR, sizeof(statusR), "밝기 %s", BRIGHT_NAME[brightMode]);

  bool colonOn = (subMs < 600);
  int  barW    = timeValid ? (int)((t.tm_sec * 1000 + subMs) * (long)barFullW / 60000L) : 0;
  bool toastOn = (millis() < toastUntil) && toastMsg[0];

  // 바뀐 게 없으면 다시 그리지 않음
  char sig[192];
  snprintf(sig, sizeof(sig), "%s:%s|%d|%d|%s|%s|%s|%d|%s|%d|%d|%d",
           hh, mm, colonOn, barW, dateStr, statusL, statusR, wifiOk, toastOn ? toastMsg : "", (int)use12h, w, h);
  if (strcmp(sig, lastSig) == 0) return;
  strlcpy(lastSig, sig, sizeof(lastSig));

  if (canvasOk) {
    canvas.fillSprite(C(0, 0, 0));
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
      if (pressX < screenW / 2) {
        use12h = !use12h;
        prefs.putBool("use12h", use12h);
        showToast(use12h ? "12시간제" : "24시간제");
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
static void onPortalStart(WiFiManager* w) {
  drawScreen("WiFi 설정 필요",
             "휴대폰 WiFi에서 'CYD-Clock' 접속",
             "자동으로 뜨는 설정 창에서",
             "집 WiFi를 선택해 주세요");
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
  brightMode = prefs.getUChar("bmode", 0);

  drawScreen("CYD 데스크 시계", "WiFi 연결 중...");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("cyd-clock");
  wm.setAPCallback(onPortalStart);
  wm.setConfigPortalTimeout(300);
  if (!wm.autoConnect(AP_NAME)) {
    drawScreen("연결 실패", "WiFi에 연결하지 못했습니다", "5초 후 다시 시작합니다");
    delay(5000);
    ESP.restart();
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
  lastSig[0] = '\0';
}

void loop() {
  handleTouch();

  static uint32_t lastBright = 0;
  if (millis() - lastBright >= 100) {
    lastBright = millis();
    updateBrightness();
  }

  drawClockFrame();
  delay(20);
}
