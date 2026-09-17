// Экраны включения/загрузки/приветствия на ILI9341 (320x240, альбомная).
// Дизайн — раскадровка в docs/boot-screen-brief.md, загрузочная анимация —
// канвас "SMPLR Boot Screen" в Claude Design (глитч-логотип + прогресс-бар).
// Рисуется примитивами Adafruit_GFX, без изображений — под ограничения
// реальной прошивки.
#pragma once

#include <stdint.h>

#include <Adafruit_ILI9341.h>

class UiScreens {
 public:
  UiScreens();

  void begin();
  void showOff();
  // percent — реальный процент готовности (0..100), не запись видео: логотип
  // и глитч перерисовываются процедурно на каждый вызов, прогресс-бар и
  // текст "N%" отражают то, что передал вызывающий код.
  void showBoot(uint8_t percent);
  // bpmFocused — селектор стоит на BPM (рамка), иначе на ряду дорожек.
  void showHome(uint16_t bpm, uint8_t track, bool bpmFocused);
  // Список пунктов меню (B.2), selected — индекс подсвеченного пункта (0..3).
  void showMenuList(uint8_t selected);
  // Заглушка страницы пункта меню — полноэкранная страница (не оверлей,
  // решено в B.2), содержимое конкретных пунктов (TEMPO/TRACK/...) отдельная
  // стори.
  void showMenuItem(uint8_t itemIndex);

 private:
  // Аппаратный SPI на "родных" пинах ESP32-S3 (SCK=12/MISO=13/MOSI=11/SS=10
  // из pins_arduino.h), через глобальный объект SPI без ремапа на GPIO-
  // матрице. Пробовали и bit-bang (слишком медленно в Wokwi — fillScreen
  // выглядел как зависшая симуляция), и аппаратный SPI на кастомных пинах
  // через SPIClass::begin(sck,miso,mosi,ss) (команды уходили без ошибок, но
  // чип экрана в Wokwi их не видел — симулятор SPI-периферии ESP32-S3
  // надёжно работает только на дефолтных пинах, не на произвольной
  // GPIO-матрице). См. CHANGELOG.md.
  Adafruit_ILI9341 tft_;
  uint8_t lastFilledSegs_ = 0;

  bool bootDrawn_ = false;
  uint8_t lastBootPercent_ = 255;
  uint8_t lastDotPhase_ = 255;
  uint32_t rngState_ = 0x5EED5EED;
  uint32_t bootFrameStart_ = 0;

  uint8_t nextRand();
  void drawHeader(const char* rightLabel, uint16_t rightColor);
  void drawBootFooter();
  void drawBootLogo();
  void drawLogoRows(int16_t xOffset, uint16_t color, uint16_t rowStart,
                     uint16_t rowCount);
  void drawBootStatus(uint8_t percent);
  void drawBootBar(uint8_t percent);
};
