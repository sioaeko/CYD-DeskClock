@echo off
chcp 65001 >nul
title CYD 데스크 시계 - 웹 플래셔
cd /d "%~dp0"

if not exist "docs\firmware-merged.bin" (
    echo [!] docs\firmware-merged.bin 이 없습니다. 먼저 빌드하세요:
    echo     python -m platformio run
    pause
    exit /b 1
)

echo.
echo  ─────────────────────────────────────────────
echo   CYD 데스크 시계 플래셔 서버를 시작합니다.
echo   브라우저(Chrome/Edge)가 자동으로 열립니다.
echo   종료하려면 이 창에서 Ctrl+C 를 누르세요.
echo  ─────────────────────────────────────────────
echo.

start "" http://localhost:8123
python -m http.server 8123 -d docs
