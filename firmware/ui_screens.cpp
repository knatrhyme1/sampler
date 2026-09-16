#include "ui_screens.h"

#include <Arduino.h>

// Пины экрана — см. firmware/diagram.json (lcd1). Нестандартные, поэтому
// программный SPI (конструктор Adafruit_ILI9341 с MOSI/SCLK явно).
namespace {
constexpr int8_t kPinCS = 11;
constexpr int8_t kPinDC = 13;
constexpr int8_t kPinRST = 12;
constexpr int8_t kPinMOSI = 14;
constexpr int8_t kPinSCK = 18;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Палитра — см. docs/boot-screen-brief.md.
constexpr uint16_t kColorBg = rgb565(0x05, 0x05, 0x06);
constexpr uint16_t kColorCream = rgb565(0xF5, 0xEF, 0xE1);
constexpr uint16_t kColorOrange = rgb565(0xFF, 0x6A, 0x2B);
constexpr uint16_t kColorGreen = rgb565(0x34, 0xC7, 0x6F);
constexpr uint16_t kColorDim = rgb565(0x3A, 0x37, 0x33);

const char* kBootStepLabels[UiScreens::kBootSteps] = {
    "SOUND ENGINE", "MPK MINI", "SEQUENCER"};

constexpr int16_t kBootListTop = 190;
constexpr int16_t kBootRowH = 26;
constexpr int16_t kBootBarY = 280;
constexpr uint8_t kBootSegments = 10;
constexpr int16_t kBootSegW = 16;
constexpr int16_t kBootSegGap = 5;
}  // namespace

UiScreens::UiScreens()
    : tft_(kPinCS, kPinDC, kPinMOSI, kPinSCK, kPinRST, -1) {}

void UiScreens::begin() {
  tft_.begin();
  tft_.setRotation(0);  // портрет, 240x320 — как в diagram.json/wokwi-ili9341
  tft_.fillScreen(ILI9341_BLACK);
}

void UiScreens::showOff() {
  tft_.fillScreen(ILI9341_BLACK);
}

void UiScreens::drawHeader(const char* rightLabel, uint16_t rightColor) {
  tft_.setTextColor(kColorCream);
  tft_.setTextSize(2);
  tft_.setCursor(20, 24);
  tft_.print("SMPLR");

  tft_.setTextSize(1);
  tft_.setTextColor(rightColor);
  int16_t textW = (int16_t)strlen(rightLabel) * 6;  // 5x7 font, size 1
  tft_.setCursor(240 - 20 - textW, 30);
  tft_.print(rightLabel);

  tft_.drawFastHLine(20, 46, 200, kColorDim);
}

void UiScreens::drawBootRow(uint8_t index, uint16_t boxColor, bool boxFilled,
                             uint16_t textColor) {
  const int16_t y = kBootListTop + index * kBootRowH;
  if (boxFilled) {
    tft_.fillRoundRect(20, y, 14, 14, 3, boxColor);
  } else {
    tft_.drawRoundRect(20, y, 14, 14, 3, boxColor);
  }
  // Текст каждой строки не меняется по содержимому, только по цвету —
  // перекрашиваем поверх тех же самых пикселей, без очистки фона.
  tft_.setTextSize(1);
  tft_.setTextColor(textColor);
  tft_.setCursor(44, y + 3);
  tft_.print(kBootStepLabels[index]);
}

void UiScreens::showBoot(uint8_t step) {
  if (step == 0) {
    // Полная перерисовка только на первом кадре загрузки.
    tft_.fillScreen(kColorBg);
    drawHeader("BOOT", kColorOrange);
    for (uint8_t i = 0; i < kBootSteps; i++) {
      drawBootRow(i, kColorDim, false, kColorDim);
    }
    for (uint8_t i = 0; i < kBootSegments; i++) {
      const int16_t x = 20 + i * (kBootSegW + kBootSegGap);
      tft_.drawRoundRect(x, kBootBarY, kBootSegW, 8, 2, kColorDim);
    }
    lastFilledSegs_ = 0;
  } else {
    // Предыдущий шаг переходит из "активного" в "готово".
    drawBootRow(step - 1, kColorGreen, true, kColorCream);
  }
  drawBootRow(step, kColorOrange, true, kColorCream);

  const uint8_t filledSegs =
      (uint8_t)(((uint32_t)(step + 1) * kBootSegments) / kBootSteps);
  for (uint8_t i = lastFilledSegs_; i < filledSegs; i++) {
    const int16_t x = 20 + i * (kBootSegW + kBootSegGap);
    tft_.fillRoundRect(x, kBootBarY, kBootSegW, 8, 2, kColorOrange);
  }
  lastFilledSegs_ = filledSegs;
}

void UiScreens::showHome(uint16_t bpm, uint8_t track) {
  tft_.fillScreen(kColorBg);
  drawHeader("READY", kColorGreen);

  char bpmStr[6];
  snprintf(bpmStr, sizeof(bpmStr), "%u", bpm);
  tft_.setTextSize(5);
  tft_.setTextColor(kColorCream);
  const int16_t bpmTextW = (int16_t)strlen(bpmStr) * 30;  // 6px*size5/char
  tft_.setCursor((240 - bpmTextW) / 2, 120);
  tft_.print(bpmStr);

  tft_.setTextSize(1);
  tft_.setTextColor(kColorOrange);
  tft_.setCursor(240 / 2 - 12, 182);
  tft_.print("BPM");

  const int16_t rowY = 230;
  const int16_t boxW = 44;
  const int16_t gap = 8;
  const int16_t startX = 20;
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

  tft_.drawFastHLine(20, 280, 200, kColorDim);
  tft_.setTextSize(1);
  tft_.setTextColor(kColorDim);
  tft_.setCursor(20, 292);
  tft_.print("PLAY-START  MODE-TRACK");
}
