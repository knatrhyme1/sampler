// Разделы проекта 2–4: микшер, пиано-ролл, аранжировка. Общая часть
// (текст, шапка, палитра) — в ui_screens.cpp и ui_screens.h.
#include <Arduino.h>

#include "ui_screens.h"

namespace {
using uicolor::kScreenW;

using uicolor::kBg;
using uicolor::kCream;
using uicolor::kOrange;
using uicolor::kDim;
using uicolor::kHint;
using uicolor::kMagenta;
using uicolor::kYellow;
using uicolor::kGlitchDim;

constexpr int16_t kTitleLineY = 40;
constexpr int16_t kFooterLineY = 204;
constexpr int16_t kHint1Y = 211;
constexpr int16_t kHint2Y = 223;

// Шапка раздела в том же стиле, что у степ-секвенсора.
void sectionTitle(Adafruit_ILI9341& tft) { tft.drawFastHLine(12, kTitleLineY, kScreenW - 24, kDim); }
}  // namespace

// ---------------------------------------------------------------------------
// Раздел 2: микшер. Шесть полос: KICK SNARE HAT PERC | MET | MASTER.
//
//   имя полосы        y=48
//   фейдер + уровень  y=62..162 (100 px = громкость 0..100)
//   значение / MUTE   y=168
//   крутилка K1..K6   y=182
namespace {
constexpr int16_t kMixX0 = 14;
constexpr int16_t kMixPitch = 50;
constexpr int16_t kMixStripW = 46;
constexpr int16_t kMixTop = 44;
constexpr int16_t kMixBottom = 196;
constexpr int16_t kMixFaderX = 9;   // от левого края полосы
constexpr int16_t kMixFaderW = 12;
constexpr int16_t kMixMeterX = 27;
constexpr int16_t kMixMeterW = 8;
constexpr int16_t kMixFaderTop = 62;
constexpr int16_t kMixFaderH = 100;

constexpr const char* kMixNames[MixerSettings::kStrips] = {"KICK", "SNARE", "HAT",
                                                           "PERC", "MET",   "MASTER"};

int16_t mixStripX(uint8_t strip) {
  // Между каналами и метрономом — небольшой зазор, мастер стоит отдельно.
  return kMixX0 + strip * kMixPitch;
}

uint16_t mixStripColor(uint8_t strip) {
  if (strip < 4) return uicolor::kTrack[strip];
  return strip == MixerSettings::kMetronomeStrip ? uicolor::kGreen : kCream;
}
}  // namespace

void UiScreens::drawMixerStripFrame(uint8_t strip, bool selected) {
  const int16_t x = mixStripX(strip);
  tft_.drawRoundRect(x - 2, kMixTop, kMixStripW + 4, kMixBottom - kMixTop, 4,
                     selected ? kOrange : kBg);
  tft_.drawRoundRect(x - 1, kMixTop + 1, kMixStripW + 2, kMixBottom - kMixTop - 2, 3,
                     selected ? kOrange : kBg);
}

void UiScreens::updateMixerStrip(const MixerSettings& mix, uint8_t strip, bool selected) {
  const int16_t x = mixStripX(strip);
  tft_.fillRect(x, kMixTop + 2, kMixStripW, kMixBottom - kMixTop - 4, kBg);
  drawMixerStripFrame(strip, selected);

  const char* name = kMixNames[strip];
  const int16_t nameW = (int16_t)strlen(name) * 6;
  drawText(x + (kMixStripW - nameW) / 2, 48, name, 1,
           selected ? kCream : kHint, kBg);

  // Фейдер: рамка и заливка снизу на громкость.
  const uint8_t vol = mix.volume[strip];
  const bool muted = mix.muted[strip];
  tft_.drawRect(x + kMixFaderX - 1, kMixFaderTop - 1, kMixFaderW + 2, kMixFaderH + 2, kDim);
  if (vol > 0) {
    tft_.fillRect(x + kMixFaderX, kMixFaderTop + kMixFaderH - vol, kMixFaderW, vol,
                  muted ? kDim : mixStripColor(strip));
  }
  tft_.drawRect(x + kMixMeterX - 1, kMixFaderTop - 1, kMixMeterW + 2, kMixFaderH + 2, kDim);
  mixerMeterPx_[strip] = kNotDrawn;
  drawMixerMeter(strip, 0);

  char value[8];
  if (muted) {
    snprintf(value, sizeof(value), "MUTE");
  } else {
    snprintf(value, sizeof(value), "%u", (unsigned)vol);
  }
  const int16_t valueW = (int16_t)strlen(value) * 6;
  drawText(x + (kMixStripW - valueW) / 2, 168, value, 1, muted ? kMagenta : kCream,
           kBg);

  char knob[4];
  snprintf(knob, sizeof(knob), "K%u", (unsigned)(strip + 1));
  drawText(x + (kMixStripW - 12) / 2, 182, knob, 1, kHint, kBg);
}

void UiScreens::drawMixerMeter(uint8_t strip, uint8_t px) {
  const uint8_t old = mixerMeterPx_[strip];
  if (px == old) return;
  const int16_t x = mixStripX(strip) + kMixMeterX;
  const int16_t bottom = kMixFaderTop + kMixFaderH;
  if (old == kNotDrawn) {
    tft_.fillRect(x, kMixFaderTop, kMixMeterW, kMixFaderH - px, kBg);
    if (px > 0) tft_.fillRect(x, bottom - px, kMixMeterW, px, kCream);
  } else if (px > old) {
    tft_.fillRect(x, bottom - px, kMixMeterW, px - old, kCream);
  } else {
    tft_.fillRect(x, bottom - old, kMixMeterW, old - px, kBg);
  }
  mixerMeterPx_[strip] = px;
}

void UiScreens::showMixer(const MixerSettings& mix, uint8_t cursor) {
  tft_.fillScreen(kBg);
  screen_ = Screen::Other;
  drawText(12, 14, "MIXER", 2, kCream, kBg);
  sectionTitle(tft_);
  for (uint8_t i = 0; i < MixerSettings::kStrips; i++) updateMixerStrip(mix, i, i == cursor);

  tft_.drawFastHLine(12, kFooterLineY, kScreenW - 24, kDim);
  drawText(12, kHint1Y, "K1-K6 VOLUME  PAD1/3 SELECT  PAD6/2 +/-", 1, kHint, kBg);
  drawText(12, kHint2Y, "PAD5 MUTE  PAD7 BACK  C3-D#3 LISTEN", 1, kHint, kBg);
}

void UiScreens::updateMixerMeters(const float* levels) {
  for (uint8_t i = 0; i < MixerSettings::kStrips; i++) {
    float l = levels[i];
    if (l < 0.0f) l = 0.0f;
    if (l > 1.0f) l = 1.0f;
    drawMixerMeter(i, (uint8_t)(l * kMixFaderH + 0.5f));
  }
}

// ---------------------------------------------------------------------------
// Раздел 3: пиано-ролл (заготовка: ноты сохраняются, но пока не звучат).
//
//   клавиатура x=4..36, 12 строк по 12 px (одна октава на экране)
//   сетка      x=44.., 16 шагов по 16 px + 3 px между тактами
namespace {
constexpr int16_t kRollKeyX = 4;
constexpr int16_t kRollKeyW = 34;
constexpr int16_t kRollGridX = 44;
constexpr int16_t kRollGridTop = 58;
constexpr int16_t kRollRowH = 11;
constexpr int16_t kRollCellW = 14;
constexpr int16_t kRollCellH = 9;
constexpr int16_t kRollStepPitch = 16;
constexpr int16_t kRollBarGap = 3;
constexpr int16_t kRollMarkerY = 50;
constexpr int16_t kRollInfoY = 194;

constexpr const char* kNoteNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                        "F#", "G",  "G#", "A",  "A#", "B"};

bool isBlackKey(uint8_t pitch) {
  const uint8_t n = (PianoRoll::kLowestNote + pitch) % 12;
  return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

void noteName(uint8_t pitch, char* out, size_t size) {
  const uint8_t midi = PianoRoll::kLowestNote + pitch;
  // MIDI 48 = C3 — так же, как нумерует клавиши MPK (docs/mpk-mapping.md).
  snprintf(out, size, "%s%d", kNoteNames[midi % 12], (int)(midi / 12) - 1);
}

int16_t rollStepX(uint8_t step) {
  return kRollGridX + step * kRollStepPitch + (step / StepSequencer::kStepsPerBar) * kRollBarGap;
}
}  // namespace

// Строка экрана для высоты pitch: вверху — самая высокая нота окна.
static int16_t rollRowY(uint8_t pitch, uint8_t viewBottom) {
  const uint8_t row = (UiScreens::kPianoVisibleRows - 1) - (pitch - viewBottom);
  return kRollGridTop + row * kRollRowH;
}

void UiScreens::drawPianoKey(uint8_t pitch, bool cursorRow) {
  const int16_t y = rollRowY(pitch, pianoViewBottom_);
  const bool black = isBlackKey(pitch);
  tft_.fillRect(kRollKeyX, y, kRollKeyW, kRollRowH - 1, black ? kDim : kHint);
  char name[6];
  noteName(pitch, name, sizeof(name));
  const bool isC = ((PianoRoll::kLowestNote + pitch) % 12) == 0;
  if (cursorRow || isC) {
    drawText(kRollKeyX + 2, y + 1, name, 1, cursorRow ? kOrange : kBg,
             black ? kDim : kHint);
  }
}

void UiScreens::updatePianoCell(const PianoRoll& roll, uint8_t step, uint8_t pitch, bool cursor) {
  if (pitch < pianoViewBottom_ || pitch >= pianoViewBottom_ + kPianoVisibleRows) return;
  const int16_t x = rollStepX(step);
  const int16_t y = rollRowY(pitch, pianoViewBottom_);
  tft_.fillRect(x - 1, y - 1, kRollCellW + 2, kRollCellH + 2, kBg);
  if (roll.isOn(step, pitch)) {
    tft_.fillRect(x, y, kRollCellW, kRollCellH, step == pianoPlayhead_ ? kCream : kYellow);
  } else if (step == pianoPlayhead_) {
    tft_.fillRect(x, y, kRollCellW, kRollCellH, kDim);
  } else {
    tft_.fillRect(x, y, kRollCellW, kRollCellH,
                  isBlackKey(pitch) ? kBg : kGlitchDim);
    if (step % StepSequencer::kStepsPerBar == 0) {
      tft_.drawFastVLine(x, y, kRollCellH, kDim);
    }
  }
  if (cursor) tft_.drawRect(x - 1, y - 1, kRollCellW + 2, kRollCellH + 2, kCream);
}

void UiScreens::drawPianoGrid(const PianoRoll& roll, uint8_t cursorStep, uint8_t cursorPitch) {
  for (uint8_t r = 0; r < kPianoVisibleRows; r++) {
    const uint8_t pitch = pianoViewBottom_ + r;
    drawPianoKey(pitch, pitch == cursorPitch);
    for (uint8_t s = 0; s < PianoRoll::kSteps; s++) {
      updatePianoCell(roll, s, pitch, s == cursorStep && pitch == cursorPitch);
    }
  }
}

void UiScreens::updatePianoCursorLabel(uint8_t cursorStep, uint8_t cursorPitch) {
  char name[6];
  noteName(cursorPitch, name, sizeof(name));
  char info[40];
  snprintf(info, sizeof(info), "%s  STEP %u  BAR %u", name, (unsigned)(cursorStep + 1),
           (unsigned)(cursorStep / StepSequencer::kStepsPerBar + 1));
  drawTextField(12, kRollInfoY, 180, info, 1, kCream, kBg);
  // Подсветка названия ноты на клавиатуре — у всех видимых строк.
  for (uint8_t r = 0; r < kPianoVisibleRows; r++) {
    const uint8_t pitch = pianoViewBottom_ + r;
    drawPianoKey(pitch, pitch == cursorPitch);
  }
}

void UiScreens::showPianoRoll(const PianoRoll& roll, uint8_t cursorStep, uint8_t cursorPitch,
                              uint8_t viewBottom, uint8_t playhead) {
  tft_.fillScreen(kBg);
  screen_ = Screen::Other;
  pianoViewBottom_ = viewBottom;
  pianoPlayhead_ = playhead;

  drawText(12, 14, "PIANO ROLL", 2, kCream, kBg);
  // Честно предупреждаем: ноты сохраняются, но инструмента под них ещё нет.
  drawText(kScreenW - 12 - 17 * 6, 20, "PREVIEW: NO SOUND", 1, kMagenta, kBg);
  sectionTitle(tft_);
  for (uint8_t bar = 0; bar < StepSequencer::kSteps / StepSequencer::kStepsPerBar; bar++) {
    const char label[2] = {(char)('1' + bar), '\0'};
    drawText(rollStepX(bar * StepSequencer::kStepsPerBar) + 3, 44, label, 1, kHint, kBg);
  }
  drawPianoGrid(roll, cursorStep, cursorPitch);
  if (playhead != StepSequencer::kNoStep) {
    tft_.fillRect(rollStepX(playhead), kRollMarkerY, kRollCellW, 3, kCream);
  }
  updatePianoCursorLabel(cursorStep, cursorPitch);

  tft_.drawFastHLine(12, kFooterLineY, kScreenW - 24, kDim);
  drawText(12, kHint1Y, "PAD6/2 PITCH  PAD1/3 STEP  PAD5 NOTE", 1, kHint, kBg);
  drawText(12, kHint2Y, "PAD7 BACK  NOTES ARE KEPT, NOT PLAYED", 1, kHint, kBg);
}

void UiScreens::updatePianoPlayhead(const PianoRoll& roll, uint8_t playhead, uint8_t cursorStep,
                                    uint8_t cursorPitch) {
  if (playhead == pianoPlayhead_) return;
  const uint8_t prev = pianoPlayhead_;
  pianoPlayhead_ = playhead;
  auto redrawColumn = [&](uint8_t step) {
    for (uint8_t r = 0; r < kPianoVisibleRows; r++) {
      const uint8_t pitch = pianoViewBottom_ + r;
      updatePianoCell(roll, step, pitch, step == cursorStep && pitch == cursorPitch);
    }
  };
  if (playhead != StepSequencer::kNoStep) {
    tft_.fillRect(rollStepX(playhead), kRollMarkerY, kRollCellW, 3, kCream);
    redrawColumn(playhead);
  }
  if (prev != StepSequencer::kNoStep) {
    tft_.fillRect(rollStepX(prev), kRollMarkerY, kRollCellW, 3, kBg);
    redrawColumn(prev);
  }
}

// ---------------------------------------------------------------------------
// Раздел 4: аранжировка (заготовка). Шкала на 16 тактов; единственный
// паттерн (4 такта) повторяется по кругу, поэтому на шкале он стоит четыре
// раза подряд. В каждой ячейке такта — мини-копия его ударов.
namespace {
constexpr uint8_t kArrRows = 5;  // KICK SNARE HAT PERC ROLL
constexpr uint8_t kArrBars = 16;
constexpr int16_t kArrX0 = 50;
constexpr int16_t kArrBarW = 16;
constexpr int16_t kArrTop = 66;
constexpr int16_t kArrRowPitch = 24;
constexpr int16_t kArrRowH = 20;
constexpr int16_t kArrMarkerY = 58;
constexpr int16_t kArrInfoY = 188;

constexpr const char* kArrRowNames[kArrRows] = {"KICK", "SNARE", "HAT", "PERC", "ROLL"};

uint16_t arrRowColor(uint8_t row) { return row < 4 ? uicolor::kTrack[row] : kYellow; }

int16_t arrBarX(uint8_t bar) { return kArrX0 + bar * kArrBarW; }
int16_t arrRowY(uint8_t row) { return kArrTop + row * kArrRowPitch; }
}  // namespace

void UiScreens::drawArrangementCell(uint8_t row, uint8_t bar, bool cursor,
                                    const StepSequencer& seq, const PianoRoll& roll) {
  const int16_t x = arrBarX(bar);
  const int16_t y = arrRowY(row);
  const uint8_t patternBar = bar % (StepSequencer::kSteps / StepSequencer::kStepsPerBar);
  tft_.fillRect(x, y, kArrBarW, kArrRowH, kGlitchDim);
  // Начало паттерна — яркая черта: видно, где блок P1 начинается заново.
  tft_.drawFastVLine(x, y, kArrRowH, patternBar == 0 ? arrRowColor(row) : kDim);
  for (uint8_t i = 0; i < StepSequencer::kStepsPerBar; i++) {
    const uint8_t step = patternBar * StepSequencer::kStepsPerBar + i;
    const bool on = row < 4 ? seq.isOn(row, step) : roll.stepHasNotes(step);
    if (on) tft_.fillRect(x + 2 + i * 3, y + 4, 2, kArrRowH - 8, arrRowColor(row));
  }
  if (cursor) tft_.drawRect(x, y, kArrBarW, kArrRowH, kCream);
}

void UiScreens::drawArrangementInfo(uint8_t cursorRow, uint8_t cursorBar) {
  char info[48];
  snprintf(info, sizeof(info), "BAR %u  %s  PATTERN P1 BAR %u", (unsigned)(cursorBar + 1),
           kArrRowNames[cursorRow], (unsigned)(cursorBar % 4 + 1));
  drawTextField(12, kArrInfoY, kScreenW - 24, info, 1, kCream, kBg);
}

void UiScreens::showArrangement(const StepSequencer& seq, const PianoRoll& roll,
                                uint8_t cursorRow, uint8_t cursorBar, uint8_t playBar) {
  tft_.fillScreen(kBg);
  screen_ = Screen::Other;
  arrPlayBar_ = kNoPlayBar;

  drawText(12, 14, "ARRANGEMENT", 2, kCream, kBg);
  drawText(kScreenW - 12 - 7 * 6, 20, "PREVIEW", 1, kMagenta, kBg);
  sectionTitle(tft_);
  for (uint8_t bar = 0; bar < kArrBars; bar += 4) {
    char label[4];
    snprintf(label, sizeof(label), "%u", (unsigned)(bar + 1));
    drawText(arrBarX(bar) + 2, 46, label, 1, kHint, kBg);
  }
  for (uint8_t r = 0; r < kArrRows; r++) {
    drawText(8, arrRowY(r) + 6, kArrRowNames[r], 1, r == cursorRow ? kCream : kHint,
             kBg);
    for (uint8_t b = 0; b < kArrBars; b++) {
      drawArrangementCell(r, b, r == cursorRow && b == cursorBar, seq, roll);
    }
  }
  drawArrangementInfo(cursorRow, cursorBar);
  updateArrangementPlayhead(playBar);

  tft_.drawFastHLine(12, kFooterLineY, kScreenW - 24, kDim);
  drawText(12, kHint1Y, "PAD1/3 BAR  PAD6/2 ROW  PAD7 BACK", 1, kHint, kBg);
  drawText(12, kHint2Y, "ONE PATTERN LOOPS - SONG MODE LATER", 1, kHint, kBg);
}

void UiScreens::updateArrangementCursor(uint8_t oldRow, uint8_t oldBar, uint8_t cursorRow,
                                        uint8_t cursorBar, const StepSequencer& seq,
                                        const PianoRoll& roll) {
  drawArrangementCell(oldRow, oldBar, false, seq, roll);
  drawArrangementCell(cursorRow, cursorBar, true, seq, roll);
  if (oldRow != cursorRow) {
    tft_.fillRect(8, arrRowY(oldRow) + 6, 36, 8, kBg);
    drawText(8, arrRowY(oldRow) + 6, kArrRowNames[oldRow], 1, kHint, kBg);
    tft_.fillRect(8, arrRowY(cursorRow) + 6, 36, 8, kBg);
    drawText(8, arrRowY(cursorRow) + 6, kArrRowNames[cursorRow], 1, kCream, kBg);
  }
  drawArrangementInfo(cursorRow, cursorBar);
}

void UiScreens::updateArrangementPlayhead(uint8_t playBar) {
  if (playBar == arrPlayBar_) return;
  if (playBar != kNoPlayBar) tft_.fillRect(arrBarX(playBar), kArrMarkerY, kArrBarW, 4, kCream);
  if (arrPlayBar_ != kNoPlayBar) tft_.fillRect(arrBarX(arrPlayBar_), kArrMarkerY, kArrBarW, 4, kBg);
  arrPlayBar_ = playBar;
}
