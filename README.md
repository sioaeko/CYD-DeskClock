# CYD 데스크 시계

ESP32-2432S028R (Cheap Yellow Display, 2.8" ILI9341 + XPT2046 터치)를 미니멀한 NTP 탁상시계로 만드는 펌웨어입니다.

## 기능

- **NTP 자동 시간 동기화** — 한국 표준시(KST), 이후 자동 재동기화
- **간편한 WiFi 설정** — 휴대폰으로 `CYD-Clock` 핫스팟에 접속해 설정 (WiFiManager 캡티브 포털)
- **새로 설계한 시계 화면** — 상단 날짜·연결 상태, 화면 폭을 채우는 고정폭 시간, 얇은 초 타임라인, 하단 날씨 스트립으로 정보 위계를 명확하게 구성
- **날씨 · 기온 · 습도** — [Open-Meteo](https://open-meteo.com/)에서 현재 날씨를 받아 아이콘·기온·습도를 표시 (무료, API 키 불필요). 위치는 IP 기반 자동이 기본이며, **웹 설정 페이지에서 도시 이름으로 직접 지정**하거나(지오코딩) `src/main.cpp`의 `MANUAL_LAT`/`MANUAL_LON`으로 고정할 수도 있습니다. 시계 렌더링을 막지 않도록 별도 코어에서 백그라운드로 갱신합니다.
- **전용 타이포그래피 3단계** — 시간은 Cascadia Mono SemiBold 96px, 기온은 Cascadia Mono Medium 30px, 한글 날짜·날씨는 Noto Sans KR Medium 17px 안티에일리어스 폰트로 표시 (모두 OFL 오픈 폰트)
- **화면 테마 8종** — 네온, 앰버, 포레스트, 모노, 오션, 선셋, 자수정, 로즈
- **야간 모드** — 밤에는 눈부심 없는 어두운 붉은 화면 + 아주 낮은 밝기로 전환. `끔`/`자동(밤 22시~오전 7시)`/`켬` 중 선택 (웹에서 설정)
- **폰으로 실시간 설정** — 같은 WiFi에서 `http://cyd-clock.local`(또는 기기 IP) 접속 시 웹 페이지에서 테마·밝기·시간표시를 바로 변경. 변경은 즉시 적용·저장됩니다. (mDNS + 내장 웹서버)
- **자동 밝기** — 내장 조도센서(LDR)로 방 밝기에 맞춰 백라이트 조절
- **WiFi 자동 재연결** — 연결이 끊겨도 시계 화면을 유지하며 백그라운드에서 재시도
- **터치 조작**
  - 화면 **왼쪽 탭**: 12/24시간제 전환
  - 화면 **가운데 탭**: 화면 테마 전환
  - 화면 **오른쪽 탭**: 밝기 모드 전환 (자동 → 최대 → 중간 → 최소)
  - **5초 길게 누르기**: 저장된 WiFi 설정 초기화 후 재시작
- 설정(12/24시간제, 테마, 밝기 모드)은 재부팅 후에도 유지됩니다.

## 폰/PC로 설정 변경

기기가 WiFi에 연결되면 부팅 화면에 접속 주소가 잠깐 표시됩니다. 같은 WiFi에 연결된 폰이나 PC 브라우저에서:

- **`http://cyd-clock.local`** — mDNS 지원 기기(대부분의 Windows/macOS/iOS/Android)에서 바로 접속
- 안 되면 부팅 화면에 표시된 **기기 IP**(예: `http://192.168.0.17`)로 접속

열리는 페이지에서 테마 8종을 실제 색상 미리보기로 고르고, 밝기 모드·12/24시간제·**야간 모드**·**날씨 지역**을 바꿀 수 있습니다. 지역은 도시 이름(예: `서울`, `부산`, `Tokyo`)을 입력하면 Open-Meteo 지오코딩으로 좌표를 찾아 적용하며, **자동(IP)** 버튼으로 되돌릴 수 있습니다. 모든 변경은 터치 조작과 동일하게 즉시 반영·저장됩니다.

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

웹 플래셔용 병합 바이너리를 다시 만들려면 (파일명은 캐시 무효화를 위해 버전을 붙입니다):

```
python -m esptool --chip esp32 merge_bin -o docs/firmware-v1.7.0.bin ^
  --flash_mode dio --flash_freq 40m --flash_size 4MB ^
  0x1000 <build>/bootloader.bin 0x8000 <build>/partitions.bin ^
  0xe000 <boot_app0.bin> 0x10000 <build>/firmware.bin
```

그런 다음 `docs/manifest.json`의 `version`과 `path`, `docs/index.html`의 버전 표기를 새 파일명에 맞춰 갱신하세요.

폰트 헤더를 다시 만들 때는 Pillow를 설치하고 `python tools/generate_fonts.py`를 실행하세요.

## 커스터마이징 (`src/main.cpp` 상단)

| 항목 | 설명 |
|---|---|
| `TZ_INFO` | 시간대 (기본 `KST-9`) |
| `AP_NAME` | WiFi 설정용 핫스팟 이름 |
| `MANUAL_LAT` / `MANUAL_LON` | 날씨 위치 좌표 고정 (기본 `0` = IP 기반 자동) |
| `CLOCK_ROTATION` | 화면 방향 (기본 `6`, 180도 뒤집히면 `4`) |
| `PANEL_INVERT` | 화면 색이 반전되어 보이는 CYD 변종이면 `true` |
| `LDR_*`, `BRIGHTNESS_*` | 자동 밝기 감도/범위 |

## 하드웨어 핀 (참고)

| 기능 | 핀 |
|---|---|
| TFT (ILI9341) | SCLK 14 / MOSI 13 / MISO 12 / CS 15 / DC 2 / BL 21 |
| 터치 (XPT2046) | SCLK 25 / MOSI 32 / MISO 39 / CS 33 / IRQ 36 |
| 조도센서 (LDR) | 34 |
| RGB LED (미사용, 꺼짐) | 4 / 16 / 17 |
