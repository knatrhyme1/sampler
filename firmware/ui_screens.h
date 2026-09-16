// Экраны включения/загрузки/приветствия на ILI9341 (240x320, портрет).
// Дизайн — раскадровка в docs/boot-screen-brief.md. Рисуется примитивами
// Adafruit_GFX, без изображений — под ограничения реальной прошивки.
#pragma once

#include <stdint.h>

#include <Adafruit_ILI9341.h>

class UiScreens {
 public:
  static const uint8_t kBootSteps = 3;

  UiScreens();

  void begin();
  void showOff();
  void showBoot(uint8_t step);  // 0..kBootSteps-1
  void showHome(uint16_t bpm, uint8_t track);

 private:
  Adafruit_ILI9341 tft_;
  uint8_t lastFilledSegs_ = 0;

  void drawHeader(const char* rightLabel, uint16_t rightColor);
  // Перерисовывает только одну строку списка шагов загрузки — полный
  // fillScreen на каждый кадр слишком медленно эмулируется в Wokwi
  // (software SPI), поэтому showBoot трогает только то, что изменилось.
  void drawBootRow(uint8_t index, uint16_t boxColor, bool boxFilled,
                    uint16_t textColor);
};
