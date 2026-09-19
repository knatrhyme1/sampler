// Экраны устройства на ILI9341 (320x240, альбомная).
// Дизайн — раскадровка в docs/archive/boot-screen-brief.md, загрузочная анимация —
// канвас "SMPLR Boot Screen" в Claude Design (глитч-логотип + прогресс-бар).
// Рисуется примитивами Adafruit_GFX, без изображений — под ограничения
// реальной прошивки.
//
// Реализация разнесена на два файла: ui_screens.cpp — включение, загрузка,
// главный экран, меню, экспорт, степ-секвенсор; ui_sections.cpp — разделы
// проекта 2–4 (микшер, пиано-ролл, аранжировка).
#pragma once

#include <stdint.h>

#include <Adafruit_ILI9341.h>

#include "audio_engine.h"
#include "piano_roll.h"
#include "step_sequencer.h"

// Версия прошивки: схема A.B.C.D описана в CHANGELOG.md.
#define SMPLR_VERSION "0.0.1.1"

// Где стоит курсор главного экрана. PAD5 "нажимает" элемент под курсором.
enum class HomeFocus : uint8_t { Bpm, Metronome, Sections };

// Разделы проекта (кнопки 1–4 на главном экране).
constexpr uint8_t kSectionCount = 4;
constexpr uint8_t kSectionSequencer = 0;
constexpr uint8_t kSectionMixer = 1;
constexpr uint8_t kSectionPianoRoll = 2;
constexpr uint8_t kSectionArrangement = 3;

// Пункты меню (B.2) в порядке показа.
constexpr uint8_t kMenuItemCount = 5;
constexpr uint8_t kMenuItemTrack = 0;
constexpr uint8_t kMenuItemTempo = 1;
constexpr uint8_t kMenuItemInput = 2;
constexpr uint8_t kMenuItemSystem = 3;
constexpr uint8_t kMenuItemExport = 4;

// Палитра — docs/archive/boot-screen-brief.md; общая для обоих файлов
// реализации.
namespace uicolor {
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t kBg = rgb565(0x05, 0x05, 0x06);
constexpr uint16_t kCream = rgb565(0xF5, 0xEF, 0xE1);
constexpr uint16_t kOrange = rgb565(0xFF, 0x6A, 0x2B);
constexpr uint16_t kGreen = rgb565(0x34, 0xC7, 0x6F);
// Рамки, неактивные заливки, линии — не для текста: контраст 1,7:1.
constexpr uint16_t kDim = rgb565(0x3A, 0x37, 0x33);
// Подсказки и неактивные подписи: 5,5:1 к фону (минимум для текста 4,5:1).
constexpr uint16_t kHint = rgb565(0x8A, 0x84, 0x78);
// Неоновая палитра глитч-экрана загрузки — точные значения из канваса
// "SMPLR Boot Screen" в Claude Design (boot-scene.jsx, объект PAL).
constexpr uint16_t kMagenta = rgb565(0xFF, 0x2B, 0xD1);
constexpr uint16_t kCyan = rgb565(0x22, 0xD8, 0xE8);
constexpr uint16_t kLime = rgb565(0xC2, 0xE8, 0x32);
constexpr uint16_t kYellow = rgb565(0xF7, 0xEC, 0x2E);
constexpr uint16_t kGlitchDim = rgb565(0x15, 0x15, 0x15);
// Цвета каналов KICK/SNARE/HAT/PERC — одни и те же во всех разделах.
constexpr uint16_t kTrack[4] = {kOrange, kCyan, kLime, kMagenta};

constexpr int16_t kScreenW = 320;
constexpr int16_t kScreenH = 240;
}  // namespace uicolor

enum class ExportStatus : uint8_t { Ready, Running, Done, Aborted, Empty };

// Всё, что показывает главный экран. Экран сам сравнивает с тем, что уже
// нарисовано, и перерисовывает только изменившиеся элементы.
struct HomeView {
  uint16_t bpm;
  uint8_t activeSection;
  uint8_t sectionCursor;
  HomeFocus focus;
  bool metronomeOn;
  bool playing;
  uint8_t bar;        // текущий такт паттерна 0..3, пока играет
  bool silent;        // играть нечего: паттерн пуст и метроном выключен
  ExportStatus exportStatus;
  uint8_t exportPercent;
  bool exportUnseen;  // экспорт закончился, а страницу EXPORT ещё не открывали
};

struct ExportView {
  ExportStatus status;
  uint8_t percent;
  uint16_t bpm;  // темп файла: экспортируемого, а не текущего
  uint32_t durationMs;
  uint32_t fileBytes;
  const char* fileName;
};

// Сводка для страницы SYSTEM.
struct SystemInfo {
  uint32_t flashKb;
  uint32_t psramKb;
  uint32_t heapFreeKb;
  uint32_t uptimeS;
};

class UiScreens {
 public:
  UiScreens();

  void begin();
  // Выключено: экран тёмный, внизу подсказка, как включить.
  void showOff();
  // Полоса удержания POWER на экране "выключено" (0 — стереть).
  void updatePowerHold(uint8_t percent);
  // percent — реальный процент готовности (0..100), не запись видео: логотип
  // и глитч перерисовываются процедурно на каждый вызов, прогресс-бар и
  // текст "N%" отражают то, что передал вызывающий код.
  void showBoot(uint8_t percent);

  // Главный экран. Если он уже на дисплее, перерисовывается только то, что
  // изменилось с прошлого вызова.
  void showHome(const HomeView& v);
  // Индикатор OUT справа: перерисовывается только изменившаяся часть полосы.
  // level (0..1) — реальный пик мастер-шины.
  void updateSoundMeter(float level);

  // --- меню ---
  // Список пунктов; если он уже на экране, перерисовываются две строки.
  void showMenuList(uint8_t selected);
  // Пункт меню, для которого ещё нет содержимого (TRACK).
  void showMenuStub(uint8_t itemIndex);
  // TEMPO: темп и метроном. row — строка под курсором (0 — BPM, 1 — MET).
  void showTempo(uint16_t bpm, bool metronomeOn, uint8_t row);
  // INPUT: скорость крутилок и последнее событие крутилки (knob 255 — ещё
  // не крутили).
  void showInput(uint8_t knobSpeed, uint8_t lastKnob, int8_t lastDelta);
  void showSystem(const SystemInfo& info);
  // EXPORT: экспорт паттерна в WAV.
  void showExport(const ExportView& v);
  // Перерисовывает только полосу прогресса и процент.
  void updateExportProgress(uint8_t percent);

  // --- раздел 1: степ-секвенсор (4 канала x 16 шагов = 4 такта четвертями) ---
  // showSequencer рисует страницу целиком, остальные методы — только
  // изменившиеся ячейки, чтобы бегущий шаг не моргал всем экраном.
  void showSequencer(const StepSequencer& seq, bool playing, uint8_t playhead,
                     uint8_t cursorTrack, uint8_t cursorStep, uint16_t bpm, bool metronomeOn);
  void updateSequencerCell(const StepSequencer& seq, uint8_t track, uint8_t step,
                           uint8_t cursorTrack, uint8_t cursorStep);
  // Курсор переехал: перерисовать старую и новую ячейку и подписи каналов.
  void updateSequencerCursor(const StepSequencer& seq, uint8_t oldTrack, uint8_t oldStep,
                             uint8_t cursorTrack, uint8_t cursorStep);
  // Бегущий шаг: зажигает новый столбец и гасит прошлый, либо убирает
  // подсветку совсем (playhead == 255, транспорт стоит).
  void updateSequencerPlayhead(const StepSequencer& seq, uint8_t playhead,
                               uint8_t cursorTrack, uint8_t cursorStep);
  void updateSequencerHeader(bool playing, uint16_t bpm, bool metronomeOn);
  // Вторая строка подсказки: обычная (confirmTrack == 255) или просьба
  // подтвердить очистку канала.
  void updateSequencerFooter(uint8_t confirmTrack);

  // --- раздел 2: микшер (ui_sections.cpp) ---
  void showMixer(const MixerSettings& mix, uint8_t cursor);
  void updateMixerStrip(const MixerSettings& mix, uint8_t strip, bool selected);
  // levels — пики полос 0..1 (каналы, метроном, мастер).
  void updateMixerMeters(const float* levels);

  // --- раздел 3: пиано-ролл (ui_sections.cpp) ---
  void showPianoRoll(const PianoRoll& roll, uint8_t cursorStep, uint8_t cursorPitch,
                     uint8_t viewBottom, uint8_t playhead);
  void updatePianoCell(const PianoRoll& roll, uint8_t step, uint8_t pitch, bool cursor);
  void updatePianoCursorLabel(uint8_t cursorStep, uint8_t cursorPitch);
  void updatePianoPlayhead(const PianoRoll& roll, uint8_t playhead, uint8_t cursorStep,
                           uint8_t cursorPitch);

  // --- раздел 4: аранжировка (ui_sections.cpp) ---
  void showArrangement(const StepSequencer& seq, const PianoRoll& roll, uint8_t cursorRow,
                       uint8_t cursorBar, uint8_t playBar);
  void updateArrangementCursor(uint8_t oldRow, uint8_t oldBar, uint8_t cursorRow,
                               uint8_t cursorBar, const StepSequencer& seq,
                               const PianoRoll& roll);
  // playBar — такт на шкале 0..15, 255 — транспорт стоит.
  void updateArrangementPlayhead(uint8_t playBar);

  // Сколько строк нот пиано-ролла видно на экране.
  static const uint8_t kPianoVisibleRows = 12;

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

  // Что сейчас нарисовано на экране. Нужен, чтобы экраны при смене курсора
  // перерисовывали только изменившиеся элементы, а не весь экран: полная
  // перерисовка в симуляции видна глазом и съедает отзывчивость
  // (docs/known-issues.md, п. 4).
  enum class Screen : uint8_t { None, Off, Home, MenuList, Tempo, Input, Other };
  Screen screen_ = Screen::None;

  // Последнее нарисованное состояние главного экрана.
  HomeView home_ = {};
  char homeStatus_[16] = "";
  char homeExport_[16] = "";

  uint8_t menuSelected_ = 0;

  // TEMPO / INPUT — что нарисовано.
  uint16_t tempoBpm_ = 0;
  bool tempoMet_ = false;
  uint8_t tempoRow_ = 0;
  uint8_t inputSpeed_ = 0;
  uint8_t inputKnob_ = 255;
  int8_t inputDelta_ = 0;

  uint8_t lastMeterFillPx_ = 255;  // 255 = ещё не рисовали, следующий вызов перерисует с нуля
  uint8_t lastPowerHoldPx_ = 0;

  bool bootDrawn_ = false;
  uint8_t lastBootPercent_ = 255;
  uint8_t lastDotPhase_ = 255;
  uint32_t rngState_ = 0x5EED5EED;
  uint32_t bootFrameStart_ = 0;

  uint8_t seqPlayheadStep_ = 255;  // 255 — столбец сейчас не подсвечен

  uint8_t mixerMeterPx_[MixerSettings::kStrips] = {};

  uint8_t pianoViewBottom_ = 0;
  uint8_t pianoPlayhead_ = 255;
  uint8_t arrPlayBar_ = 255;

  uint8_t nextRand();
  // Строка шрифтом Adafruit_GFX (5x7, size — масштаб) на сплошном фоне bg.
  // Рисуется в канвас в RAM и уходит на экран одним окном адресов.
  void drawText(int16_t x, int16_t y, const char* text, uint8_t size, uint16_t fg, uint16_t bg);
  // То же, но сначала стирает полосу шириной w под строкой (для значений,
  // длина которых меняется).
  void drawTextField(int16_t x, int16_t y, int16_t w, const char* text, uint8_t size, uint16_t fg,
                     uint16_t bg);
  void drawHomeBpm(uint16_t bpm, bool focused);
  void drawHomeMetronome(bool on, bool focused);
  void drawHomeSection(uint8_t section, bool active, bool cursor);
  void drawHomeStatus(const HomeView& v);
  void drawHomeExportBadge(const HomeView& v);
  void drawMenuRow(uint8_t item, bool selected);
  void drawPageFrame(const char* title, const char* hint1, const char* hint2);
  void drawTempoRow(uint8_t row, uint16_t bpm, bool metronomeOn, bool selected);
  void drawInputValues(uint8_t knobSpeed, uint8_t lastKnob, int8_t lastDelta);
  void drawSequencerCell(const StepSequencer& seq, uint8_t track, uint8_t step,
                         bool cursor);
  void drawSequencerLabel(uint8_t track, bool selected);
  void drawSequencerPlayheadColumn(const StepSequencer& seq, uint8_t step, bool lit,
                                   uint8_t cursorTrack, uint8_t cursorStep);
  void drawMixerStripFrame(uint8_t strip, bool selected);
  void drawMixerMeter(uint8_t strip, uint8_t px);
  void drawPianoKey(uint8_t pitch, bool cursorRow);
  void drawPianoGrid(const PianoRoll& roll, uint8_t cursorStep, uint8_t cursorPitch);
  void drawArrangementCell(uint8_t row, uint8_t bar, bool cursor, const StepSequencer& seq,
                           const PianoRoll& roll);
  void drawArrangementInfo(uint8_t cursorRow, uint8_t cursorBar);
  void drawHeader(const char* rightLabel, uint16_t rightColor);
  void drawBootFooter();
  void drawBootLogo();
  void drawLogoRows(int16_t xOffset, uint16_t color, uint16_t rowStart,
                     uint16_t rowCount);
  void drawBootStatus(uint8_t percent);
  void drawBootBar(uint8_t percent);
};
