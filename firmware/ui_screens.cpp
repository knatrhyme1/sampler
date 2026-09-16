#include "ui_screens.h"

#include <Arduino.h>

#include "logo_bitmap.h"

// Пины экрана — см. firmware/diagram.json (lcd1). CS/DC/RST — любые
// свободные GPIO, а MOSI/SCK нарочно совпадают с дефолтным аппаратным SPI
// ESP32-S3 (см. UiScreens.h, почему).
namespace {
constexpr int8_t kPinCS = 10;
constexpr int8_t kPinDC = 14;
constexpr int8_t kPinRST = 18;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Альбомная ориентация: setRotation(1) в begin() даёт width()=320,
// height()=240 — см. также "rotate" у lcd1 в diagram.json (должен
// физически развернуть деталь на экране симуляции в ту же сторону).
constexpr int16_t kScreenW = 320;
constexpr int16_t kScreenH = 240;

// Базовая палитра — см. docs/boot-screen-brief.md.
constexpr uint16_t kColorBg = rgb565(0x05, 0x05, 0x06);
constexpr uint16_t kColorCream = rgb565(0xF5, 0xEF, 0xE1);
constexpr uint16_t kColorOrange = rgb565(0xFF, 0x6A, 0x2B);
constexpr uint16_t kColorGreen = rgb565(0x34, 0xC7, 0x6F);
constexpr uint16_t kColorDim = rgb565(0x3A, 0x37, 0x33);

// Неоновая палитра глитч-экрана загрузки — точные значения из канваса
// "SMPLR Boot Screen" в Claude Design (boot-scene.jsx, объект PAL).
constexpr uint16_t kColorMagenta = rgb565(0xFF, 0x2B, 0xD1);
constexpr uint16_t kColorCyan = rgb565(0x22, 0xD8, 0xE8);
constexpr uint16_t kColorLime = rgb565(0xC2, 0xE8, 0x32);
constexpr uint16_t kColorYellow = rgb565(0xF7, 0xEC, 0x2E);
constexpr uint16_t kColorGlitchDim = rgb565(0x15, 0x15, 0x15);
// CYCLE = [mag, cyan, lime, yel] — порядок цветов прогресс-бара и конфетти.
constexpr uint16_t kGlitchPalette[4] = {kColorMagenta, kColorCyan, kColorLime,
                                         kColorYellow};

// Логотип — растровый спрайт (firmware/logo_bitmap.h, сгенерирован
// tools/png_to_bitmap.py из smplr-logo.png, тот же файл, что в канвасе),
// а не текст: 320x208, уже обрезан/отмасштабирован под экран 320x240 так же,
// как top=-32 в boot-scene.jsx (видимая часть логотипа не долезает до строки
// статуса и бара).
constexpr int16_t kLogoTop = 0;

constexpr int16_t kStatusY = 168;
constexpr uint8_t kBootBarSegments = 20;
constexpr int16_t kBarX = 21;
constexpr int16_t kBarY = 186;
constexpr int16_t kBarSegW = 12;
constexpr int16_t kBarSegH = 10;
constexpr int16_t kBarGap = 2;
constexpr int16_t kFooterY = 214;
}  // namespace

UiScreens::UiScreens()
    : tft_(kPinCS, kPinDC, kPinRST) {}

void UiScreens::begin() {
  tft_.begin();
  tft_.setRotation(1);  // альбомная, 320x240 — см. "rotate" у lcd1 в diagram.json
  tft_.fillScreen(ILI9341_BLACK);
}

void UiScreens::showOff() {
  tft_.fillScreen(ILI9341_BLACK);
  bootDrawn_ = false;  // следующий showBoot() снова нарисует экран с нуля
}

uint8_t UiScreens::nextRand() {
  // xorshift32 — детерминированный ГПСЧ, достаточно "случайный" на вид для
  // разлёта глитч-пикселей, не требует истинной энтропии.
  rngState_ ^= rngState_ << 13;
  rngState_ ^= rngState_ >> 17;
  rngState_ ^= rngState_ << 5;
  return (uint8_t)(rngState_ & 0xFF);
}

void UiScreens::drawHeader(const char* rightLabel, uint16_t rightColor) {
  tft_.setTextColor(kColorCream);
  tft_.setTextSize(2);
  tft_.setCursor(20, 24);
  tft_.print("SMPLR");

  tft_.setTextSize(1);
  tft_.setTextColor(rightColor);
  int16_t textW = (int16_t)strlen(rightLabel) * 6;  // 5x7 font, size 1
  tft_.setCursor(kScreenW - 20 - textW, 30);
  tft_.print(rightLabel);

  tft_.drawFastHLine(20, 46, kScreenW - 40, kColorDim);
}

void UiScreens::drawBootFooter() {
  tft_.setTextSize(1);
  tft_.setTextColor(kColorMagenta);
  tft_.setCursor(21, kFooterY);
  tft_.print("PRERELISE V0.1");

  const char* right = "SMPLR OS";
  const int16_t textW = (int16_t)strlen(right) * 6;
  tft_.setTextColor(kColorCyan);
  tft_.setCursor(kScreenW - 21 - textW, kFooterY);
  tft_.print(right);
}

void UiScreens::drawLogoRows(int16_t xOffset, uint16_t color,
                              uint16_t rowStart, uint16_t rowCount) {
  const uint16_t rowBytes = (kSmplrLogoWidth + 7) / 8;
  const uint16_t rowEnd =
      (uint16_t)min((int32_t)rowStart + rowCount, (int32_t)kSmplrLogoHeight);
  for (uint16_t srcRow = rowStart; srcRow < rowEnd; srcRow++) {
    const int16_t y = kLogoTop + srcRow;
    if (y < 0 || y >= kScreenH) continue;
    int16_t runStart = -1;
    for (uint16_t x = 0; x <= kSmplrLogoWidth; x++) {
      bool on = false;
      if (x < kSmplrLogoWidth) {
        const uint8_t b =
            pgm_read_byte(&kSmplrLogo[srcRow * rowBytes + x / 8]);
        on = (b >> (7 - (x % 8))) & 1;
      }
      if (on && runStart < 0) {
        runStart = (int16_t)x;
      } else if (!on && runStart >= 0) {
        tft_.drawFastHLine(xOffset + runStart, y, (int16_t)x - runStart,
                            color);
        runStart = -1;
      }
    }
  }
}

void UiScreens::drawBootLogo() {
  // Базовый спрайт логотипа рисуется один раз при входе в экран загрузки
  // (см. showBoot) — полная перерисовка мелкого хатч-узора на каждый кадр
  // означала тысячи крошечных SPI-команд за кадр, которые симулятор Wokwi
  // не успевал передать целиком, и кадр рвался (часть экрана — старый кадр,
  // часть — новый). Здесь — только короткие вспышки глитча поверх уже
  // нарисованного логотипа (полосы-слайсы со сдвигом + цветные
  // пиксели-конфетти, ~140мс раз в ~500мс — совпадает с логикой burst() в
  // boot-scene.jsx), это маленькие правки, а не перерисовка всей области.
  const uint32_t elapsed = millis() - bootFrameStart_;
  if ((elapsed % 500) >= 140) return;  // вне вспышки — ничего не трогаем

  for (uint8_t i = 0; i < 2; i++) {
    const uint16_t top = (uint16_t)(nextRand() % kSmplrLogoHeight);
    const uint16_t h = 4 + (nextRand() % 10);
    const int16_t dx = (int16_t)(nextRand() % 9) - 4;  // ±4px
    drawLogoRows(dx, kColorCream, top, h);
  }
  for (uint8_t i = 0; i < 3; i++) {
    const int16_t bx = (int16_t)(nextRand() % kScreenW);
    const int16_t by = kLogoTop + (int16_t)(nextRand() % kSmplrLogoHeight);
    const uint8_t sz = 2 + (nextRand() % 3);
    tft_.fillRect(bx, by, sz * 2, sz, kGlitchPalette[nextRand() % 4]);
  }
}

void UiScreens::drawBootStatus(uint8_t percent) {
  tft_.fillRect(21, kStatusY - 2, kScreenW - 42, 12, kColorBg);

  tft_.setTextSize(1);
  tft_.setTextColor(kColorLime);
  tft_.setCursor(21, kStatusY);
  tft_.print("BOOTING");
  const uint8_t dots = (uint8_t)((millis() / 400) % 4);
  for (uint8_t i = 0; i < dots; i++) {
    tft_.print(".");
  }

  char pctStr[6];
  snprintf(pctStr, sizeof(pctStr), "%u%%", percent);
  tft_.setTextColor(kColorCream);
  const int16_t textW = (int16_t)strlen(pctStr) * 6;  // 6px*size1/char
  tft_.setCursor(kScreenW - 21 - textW, kStatusY);
  tft_.print(pctStr);
}

void UiScreens::drawBootBar(uint8_t percent) {
  const uint8_t filled =
      (uint8_t)(((uint16_t)percent * kBootBarSegments) / 100);
  for (uint8_t i = lastFilledSegs_; i < filled; i++) {
    const int16_t x = kBarX + i * (kBarSegW + kBarGap);
    tft_.fillRect(x, kBarY, kBarSegW, kBarSegH, kGlitchPalette[i % 4]);
  }
  lastFilledSegs_ = filled;
}

void UiScreens::showBoot(uint8_t percent) {
  if (!bootDrawn_) {
    tft_.fillScreen(kColorBg);
    drawLogoRows(0, kColorCream, 0, kSmplrLogoHeight);
    drawBootFooter();
    for (uint8_t i = 0; i < kBootBarSegments; i++) {
      const int16_t x = kBarX + i * (kBarSegW + kBarGap);
      tft_.fillRect(x, kBarY, kBarSegW, kBarSegH, kColorGlitchDim);
    }
    lastFilledSegs_ = 0;
    lastBootPercent_ = 255;
    lastDotPhase_ = 255;
    bootFrameStart_ = millis();
    bootDrawn_ = true;
  }

  drawBootLogo();  // каждый кадр — глитч анимируется постоянно

  const uint8_t dotPhase = (uint8_t)((millis() / 400) % 4);
  if (percent != lastBootPercent_ || dotPhase != lastDotPhase_) {
    drawBootStatus(percent);
    lastDotPhase_ = dotPhase;
  }
  if (percent != lastBootPercent_) {
    drawBootBar(percent);
    lastBootPercent_ = percent;
  }
}

void UiScreens::showHome(uint16_t bpm, uint8_t track) {
  tft_.fillScreen(kColorBg);
  drawHeader("READY", kColorGreen);

  char bpmStr[6];
  snprintf(bpmStr, sizeof(bpmStr), "%u", bpm);
  tft_.setTextSize(4);
  tft_.setTextColor(kColorCream);
  const int16_t bpmTextW = (int16_t)strlen(bpmStr) * 24;  // 6px*size4/char
  tft_.setCursor((kScreenW - bpmTextW) / 2, 58);
  tft_.print(bpmStr);

  tft_.setTextSize(1);
  tft_.setTextColor(kColorOrange);
  tft_.setCursor(kScreenW / 2 - 12, 100);
  tft_.print("BPM");

  const int16_t rowY = 122;
  const int16_t boxW = 44;
  const int16_t gap = 8;
  const int16_t startX = (kScreenW - (4 * boxW + 3 * gap)) / 2;
  for (uint8_t i = 0; i < 4; i++) {
    const int16_t x = startX + i * (boxW + gap);
    if (i == track) {
      tft_.fillRoundRect(x, rowY, boxW, 32, 5, kColorOrange);
      tft_.setTextColor(kColorBg);
    } else {
      tft_.drawRoundRect(x, rowY, boxW, 32, 5, kColorDim);
      tft_.setTextColor(kColorDim);
    }
    tft_.setTextSize(2);
    tft_.setCursor(x + boxW / 2 - 6, rowY + 8);
    tft_.print(i + 1);
  }

  tft_.drawFastHLine(20, 176, kScreenW - 40, kColorDim);
  tft_.setTextSize(1);
  tft_.setTextColor(kColorDim);
  tft_.setCursor(20, 188);
  tft_.print("PLAY-START  MODE-TRACK");
}
