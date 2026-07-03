# CYD 데스크 시계

ESP32-2432S028R (Cheap Yellow Display, 2.8" ILI9341 + XPT2046 터치)를 미니멀한 NTP 탁상시계로 만드는 펌웨어입니다.

## 기능

- **NTP 자동 시간 동기화** — 한국 표준시(KST), 이후 자동 재동기화
- **간편한 WiFi 설정** — 휴대폰으로 `CYD-Clock` 핫스팟에 접속해 설정 (WiFiManager 캡티브 포털)
- **깔끔한 시계 화면** — 큰 시간 표시, 한글 날짜/요일, 초 진행 바, 콜론 깜빡임
- **자동 밝기** — 내장 조도센서(LDR)로 방 밝기에 맞춰 백라이트 조절
- **터치 조작**
  - 화면 **왼쪽 탭**: 12/24시간제 전환
  - 화면 **오른쪽 탭**: 밝기 모드 전환 (자동 → 최대 → 중간 → 최소)
  - **5초 길게 누르기**: 저장된 WiFi 설정 초기화 후 재시작
- 설정(12/24시간제, 밝기 모드)은 재부팅 후에도 유지됩니다.

## 가장 쉬운 설치: 웹 플래셔

**온라인**: <https://sioaeko.github.io/CYD-DeskClock/> 에 접속하면 빌드 없이 바로 설치할 수 있습니다.

로컬에서 쓰려면:

1. `flash.cmd` 더블클릭 → 브라우저가 자동으로 열립니다 (Chrome/Edge 필요).
2. CYD를 USB로 연결하고 **"기기에 설치하기"** 클릭 → 포트(CH340) 선택.
3. 설치가 끝나면 기기 화면 안내에 따라 휴대폰으로 WiFi를 설정합니다.

포트가 안 보이면 [CH340 드라이버](https://www.wch-ic.com/downloads/CH341SER_ZIP.html)를 설치하세요.

## 직접 빌드 / 업로드 (PlatformIO)

```
python -m pip install platformio     # 최초 1회
python -m platformio run             # 빌드
python -m platformio run -t upload   # USB로 바로 업로드
```

빌드 산출물은 `C:/Users/Public/pio-build/cyd-deskclock/cyd/` 에 생성됩니다.
(홈 폴더 경로에 한글이 포함되어 있어 툴체인 오류를 피하려고 `platformio.ini`의
`core_dir`/`build_dir`를 ASCII 경로로 지정했습니다. 다른 PC에서는 지워도 됩니다.)

웹 플래셔용 병합 바이너리를 다시 만들려면:

```
python -m esptool --chip esp32 merge_bin -o docs/firmware-merged.bin ^
  --flash_mode dio --flash_freq 40m --flash_size 4MB ^
  0x1000 <build>/bootloader.bin 0x8000 <build>/partitions.bin ^
  0xe000 <boot_app0.bin> 0x10000 <build>/firmware.bin
```

## 커스터마이징 (`src/main.cpp` 상단)

| 항목 | 설명 |
|---|---|
| `TZ_INFO` | 시간대 (기본 `KST-9`) |
| `AP_NAME` | WiFi 설정용 핫스팟 이름 |
| `CLOCK_ROTATION` | 화면 방향 (기본 `1`, 180도 뒤집히면 `3`) |
| `PANEL_INVERT` | 화면 색이 반전되어 보이는 CYD 변종이면 `true` |
| `LDR_*`, `BRIGHTNESS_*` | 자동 밝기 감도/범위 |

## 하드웨어 핀 (참고)

| 기능 | 핀 |
|---|---|
| TFT (ILI9341) | SCLK 14 / MOSI 13 / MISO 12 / CS 15 / DC 2 / BL 21 |
| 터치 (XPT2046) | SCLK 25 / MOSI 32 / MISO 39 / CS 33 / IRQ 36 |
| 조도센서 (LDR) | 34 |
| RGB LED (미사용, 꺼짐) | 4 / 16 / 17 |
