@echo off
chcp 65001 >nul
setlocal
if "%~1"=="" (
  echo Перетащи на этот файл .txt с сохранённым текстом Serial Monitor из Wokwi.
  echo.
  pause
  exit /b 1
)
python "%~dp0wav_from_serial.py" %*
if errorlevel 9009 (
  echo.
  echo Не нашёлся python. Установи Python 3 с python.org и поставь галочку "Add to PATH".
)
echo.
pause
