// Экраны включения/загрузки/приветствия на ILI9341 (320x240, альбомная).
// Дизайн — раскадровка в docs/boot-screen-brief.md, загрузочная анимация —
// канвас "SMPLR Boot Screen" в Claude Design (глитч-логотип + прогресс-бар).
// Рисуется примитивами Adafruit_GFX, без изображений — под ограничения
// реальной прошивки.
#pragma once

#include <stdint.h>

#include <Adafruit_ILI9341.h>

#include "step_sequencer.h"

// Где стоит курсор главного экрана. PAD5 "нажимает" элемент под курсором.
enum class HomeFocus : uint8_t { Bpm, Metronome, Sections };

// Пункты меню (B.2) в порядке показа.
constexpr uint8_t kMenuItemCount = 5;
constexpr uint8_t kMenuItemExport = 4;

enum class ExportStatus : uint8_t { Ready, Running, Done, Aborted };

class UiScreens {
 public:
  UiScreens();

  void begin();
  void showOff();
  // percent — реальный процент готовности (0..100), не запись видео: логотип
  // и глитч перерисовываются процедурно на каждый вызов, прогресс-бар и
  // текст "N%" отражают то, что передал вызывающий код.
  void showBoot(uint8_t percent);
  // Кнопки 1–4 — разделы проекта (навигация внутри проекта), не дорожки.
  // activeSection — выбранный (нажатый PAD5) раздел, всегда залит.
  // sectionCursor — раздел под курсором, обводится рамкой, только пока
  // focus == HomeFocus::Sections.
  // metronomeOn — настройка "метроном щёлкает при проигрывании", а не факт
  // щелчков. playing — транспорт проекта (PLAY), показывается в шапке.
  // Если главный экран уже на дисплее, перерисовывается только то, что
  // изменилось с прошлого вызова (BPM, метроном, рамки разделов).
  void showHome(uint16_t bpm, uint8_t activeSection, uint8_t sectionCursor, HomeFocus focus,
                bool metronomeOn, bool playing);
  // Перерисовывает только статус транспорта в шапке главного экрана.
  void updateHomeTransport(bool playing);
  // Перерисовывает только изменившуюся часть полосы индикатора уровня
  // сигнала справа (без fillScreen всего экрана — иначе моргает на каждый
  // кадр). level (0..1) —
  // реальное значение, посчитанное в firmware.ino по фактическому состоянию
  // ШИМ-канала метронома, а не декоративная анимация.
  void updateSoundMeter(float level);
  // Страница EXPORT: экспорт паттерна в WAV. durationMs/fileBytes — размер
  // будущего (или отправленного) файла, percent — ход экспорта.
  void showExport(ExportStatus status, uint8_t percent, uint16_t bpm, uint32_t durationMs,
                  uint32_t fileBytes, const char* fileName);
  // Перерисовывает только полосу прогресса и процент.
  void updateExportProgress(uint8_t percent);
  // Список пунктов меню (B.2), selected — индекс подсвеченного пункта.
  // Если список уже на дисплее, перерисовываются только две строки —
  // бывшая и новая подсвеченная.
  void showMenuList(uint8_t selected);
  // Заглушка страницы пункта меню — полноэкранная страница (не оверлей,
  // решено в B.2), содержимое конкретных пунктов (TEMPO/TRACK/...) отдельная
  // стори.
  void showMenuItem(uint8_t itemIndex);
  // Заглушка страницы раздела проекта (section 0..3).
  void showSectionPage(uint8_t section);

  // Раздел 1 — степ-секвенсор: сетка 4 канала x 16 шагов (2 такта восьмыми).
  // showSequencer рисует страницу целиком, остальные методы — только
  // изменившиеся ячейки, чтобы бегущий шаг не моргал всем экраном.
  // playing/playhead приходят от транспорта: часы живут в задаче звука,
  // паттерн о времени больше ничего не знает.
  void showSequencer(const StepSequencer& seq, bool playing, uint8_t playhead,
                     uint8_t cursorTrack, uint8_t cursorStep, uint16_t bpm);
  void updateSequencerCell(const StepSequencer& seq, uint8_t track, uint8_t step,
                           uint8_t cursorTrack, uint8_t cursorStep);
  // Курсор переехал: перерисовать старую и новую ячейку и подписи каналов.
  void updateSequencerCursor(const StepSequencer& seq, uint8_t oldTrack, uint8_t oldStep,
                             uint8_t cursorTrack, uint8_t cursorStep);
  // Бегущий шаг: гасит подсветку прошлого столбца и зажигает playhead,
  // либо убирает подсветку совсем (playhead == 255, транспорт стоит).
  void updateSequencerPlayhead(const StepSequencer& seq, uint8_t playhead,
                               uint8_t cursorTrack, uint8_t cursorStep);
  void updateSequencerTransport(bool playing, uint16_t bpm);

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

  // Что сейчас нарисовано на экране. Нужен, чтобы showHome()/showMenuList()
  // при смене курсора перерисовывали только изменившиеся элементы, а не
  // весь экран: полная перерисовка в симуляции видна глазом и съедает
  // отзывчивость (docs/known-issues.md, п. 4).
  enum class Screen : uint8_t { None, Home, MenuList, Other };
  Screen screen_ = Screen::None;

  // Последнее нарисованное состояние главного экрана.
  uint16_t homeBpm_ = 0;
  uint8_t homeActiveSection_ = 0;
  uint8_t homeSectionCursor_ = 0;
  HomeFocus homeFocus_ = HomeFocus::Sections;
  bool homeMetronomeOn_ = false;
  bool homePlaying_ = false;

  uint8_t menuSelected_ = 0;

  uint8_t lastMeterFillPx_ = 255;  // 255 = ещё не рисовали, следующий вызов перерисует с нуля

  bool bootDrawn_ = false;
  uint8_t lastBootPercent_ = 255;
  uint8_t lastDotPhase_ = 255;
  uint32_t rngState_ = 0x5EED5EED;
  uint32_t bootFrameStart_ = 0;

  uint8_t seqPlayheadStep_ = 255;  // 255 — столбец сейчас не подсвечен

  uint8_t nextRand();
  // Строка шрифтом Adafruit_GFX (5x7, size — масштаб) на сплошном фоне bg.
  // Рисуется в канвас в RAM и уходит на экран одним окном адресов.
  void drawText(int16_t x, int16_t y, const char* text, uint8_t size, uint16_t fg, uint16_t bg);
  void drawHomeBpm(uint16_t bpm, bool focused);
  void drawHomeMetronome(bool on, bool focused);
  void drawHomeSection(uint8_t section, bool active, bool cursor);
  void drawMenuRow(uint8_t item, bool selected);
  void drawSequencerCell(const StepSequencer& seq, uint8_t track, uint8_t step,
                         bool cursor);
  void drawSequencerLabel(uint8_t track, bool selected);
  void drawSequencerPlayheadColumn(const StepSequencer& seq, uint8_t step, bool lit,
                                   uint8_t cursorTrack, uint8_t cursorStep);
  void drawHeader(const char* rightLabel, uint16_t rightColor);
  void drawBootFooter();
  void drawBootLogo();
  void drawLogoRows(int16_t xOffset, uint16_t color, uint16_t rowStart,
                     uint16_t rowCount);
  void drawBootStatus(uint8_t percent);
  void drawBootBar(uint8_t percent);
};
