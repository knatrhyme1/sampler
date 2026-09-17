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

// Главный экран (B.2 + метроном): слева BPM/метроном/разделы проекта в колонке
// шириной kHomeLeftW, справа — вертикальный индикатор уровня сигнала во всю
// высоту рабочей области экрана.
constexpr int16_t kHomeLeftW = 220;
constexpr int16_t kMeterX = 244;
constexpr int16_t kMeterW = 56;
constexpr int16_t kMeterY = 54;
constexpr int16_t kMeterBottom = 214;
constexpr int16_t kMeterH = kMeterBottom - kMeterY;
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

void UiScreens::showHome(uint16_t bpm, uint8_t activeSection, uint8_t sectionCursor,
                          HomeFocus focus, bool metronomeOn, bool playing) {
  tft_.fillScreen(kColorBg);
  drawHeader("", kColorGreen);
  updateHomeTransport(playing);

  const bool bpmValueFocused = focus == HomeFocus::Bpm;
  const bool metronomeFocused = focus == HomeFocus::Metronome;
  const int16_t bpmX = 28;

  char bpmStr[6];
  snprintf(bpmStr, sizeof(bpmStr), "%u", bpm);
  tft_.setTextSize(4);
  tft_.setTextColor(kColorCream);
  const int16_t bpmTextW = (int16_t)strlen(bpmStr) * 24;  // 6px*size4/char
  tft_.setCursor(bpmX, 58);
  tft_.print(bpmStr);

  if (bpmValueFocused) {
    const int16_t frameW = max(bpmTextW, (int16_t)48) + 20;
    tft_.drawRoundRect(bpmX - 10, 51, frameW, 46, 6, kColorOrange);
    tft_.drawRoundRect(bpmX - 9, 52, frameW - 2, 44, 5, kColorOrange);
  }

  tft_.setTextSize(1);
  tft_.setTextColor(kColorOrange);
  tft_.setCursor(bpmX, 104);
  tft_.print("BPM");

  // Кнопка метронома — справа от темпа, в той же строке.
  const int16_t metX = 152;
  const int16_t metY = 51;
  const int16_t metW = 56;
  const int16_t metH = 46;
  if (metronomeOn) {
    tft_.fillRoundRect(metX, metY, metW, metH, 6, kColorGreen);
    tft_.setTextColor(kColorBg);
  } else {
    tft_.drawRoundRect(metX, metY, metW, metH, 6, kColorDim);
    tft_.setTextColor(kColorDim);
  }
  tft_.setTextSize(1);
  tft_.setCursor(metX + 9, metY + 12);
  tft_.print("MET");
  tft_.setCursor(metX + 9, metY + 26);
  tft_.print(metronomeOn ? "ON" : "OFF");
  if (metronomeFocused) {
    tft_.drawRoundRect(metX - 3, metY - 3, metW + 6, metH + 6, 8, kColorOrange);
  }

  const int16_t rowY = 140;
  const int16_t boxW = 40;
  const int16_t gap = 6;
  const int16_t startX = (kHomeLeftW - (4 * boxW + 3 * gap)) / 2;
  for (uint8_t i = 0; i < 4; i++) {
    const int16_t x = startX + i * (boxW + gap);
    if (i == activeSection) {
      tft_.fillRoundRect(x, rowY, boxW, 32, 5, kColorOrange);
      tft_.setTextColor(kColorBg);
    } else {
      tft_.drawRoundRect(x, rowY, boxW, 32, 5, kColorDim);
      tft_.setTextColor(kColorDim);
    }
    tft_.setTextSize(2);
    tft_.setCursor(x + boxW / 2 - 6, rowY + 8);
    tft_.print(i + 1);
    if (focus == HomeFocus::Sections && i == sectionCursor) {
      tft_.drawRoundRect(x - 3, rowY - 3, boxW + 6, 38, 7, kColorCream);
    }
  }

  tft_.drawFastHLine(20, 176, kHomeLeftW - 20, kColorDim);
  tft_.setTextSize(1);
  tft_.setTextColor(kColorDim);
  tft_.setCursor(20, 188);
  tft_.print("PAD1/2/3/6-MOVE PAD5-PRESS");
  tft_.setCursor(20, 198);
  tft_.print("MODE-MENU");

  // Рамка индикатора уровня сигнала — статичная часть; саму заливку по
  // кадрам рисует updateSoundMeter(), чтобы не перерисовывать весь экран.
  tft_.drawRoundRect(kMeterX - 4, kMeterY - 4, kMeterW + 8, kMeterH + 8, 4, kColorDim);
  tft_.setTextColor(kColorDim);
  tft_.setCursor(kMeterX, 222);
  tft_.print("OUT");
  lastMeterFillPx_ = 255;
  updateSoundMeter(0.0f);
}

void UiScreens::updateHomeTransport(bool playing) {
  const char* label = playing ? "PLAY" : "READY";
  const int16_t textW = (int16_t)strlen(label) * 6;
  const int16_t rightX = kScreenW - 20;
  tft_.fillRect(rightX - 60, 26, 60, 12, kColorBg);
  tft_.setTextSize(1);
  tft_.setTextColor(kColorGreen);
  tft_.setCursor(rightX - textW, 30);
  tft_.print(label);
  if (playing) {
    tft_.fillTriangle(rightX - textW - 12, 29, rightX - textW - 12, 37, rightX - textW - 5, 33,
                      kColorGreen);
  }
}

void UiScreens::updateSoundMeter(float level) {
  if (level < 0.0f) level = 0.0f;
  if (level > 1.0f) level = 1.0f;

  const uint8_t fillPx = (uint8_t)(level * kMeterH + 0.5f);
  if (fillPx == lastMeterFillPx_) return;

  // Перерисовываем только изменившуюся часть полосы: сверху фон до уровня
  // заливки, снизу — сама заливка. Так индикатор не моргает на каждый кадр.
  tft_.fillRect(kMeterX, kMeterY, kMeterW, kMeterH - fillPx, kColorBg);
  if (fillPx > 0) {
    tft_.fillRect(kMeterX, kMeterY + (kMeterH - fillPx), kMeterW, fillPx, kColorOrange);
  }
  lastMeterFillPx_ = fillPx;
}

namespace {
constexpr const char* kMenuItems[kMenuItemCount] = {"TRACK", "TEMPO", "INPUT", "SYSTEM",
                                                    "EXPORT"};

constexpr int16_t kExportBarX = 20;
constexpr int16_t kExportBarY = 150;
constexpr int16_t kExportBarW = kScreenW - 40;
constexpr int16_t kExportBarH = 14;
}  // namespace

void UiScreens::showMenuList(uint8_t selected) {
  tft_.fillScreen(kColorBg);
  drawHeader("MENU", kColorOrange);

  const int16_t rowY0 = 58;
  const int16_t rowH = 30;
  for (uint8_t i = 0; i < kMenuItemCount; i++) {
    const int16_t y = rowY0 + i * rowH;
    if (i == selected) {
      tft_.fillRoundRect(20, y, kScreenW - 40, rowH - 8, 5, kColorOrange);
      tft_.setTextColor(kColorBg);
    } else {
      tft_.drawRoundRect(20, y, kScreenW - 40, rowH - 8, 5, kColorDim);
      tft_.setTextColor(kColorCream);
    }
    tft_.setTextSize(2);
    tft_.setCursor(32, y + 5);
    tft_.print(kMenuItems[i]);
  }

  tft_.drawFastHLine(20, 214, kScreenW - 40, kColorDim);
  tft_.setTextSize(1);
  tft_.setTextColor(kColorDim);
  tft_.setCursor(20, 222);
  tft_.print("UP/DOWN PAD6/2  ENTER PAD3/5  BACK PAD1/7");
}

void UiScreens::showMenuItem(uint8_t itemIndex) {
  tft_.fillScreen(kColorBg);
  drawHeader(kMenuItems[itemIndex], kColorOrange);

  tft_.setTextSize(1);
  tft_.setTextColor(kColorDim);
  tft_.setCursor(20, 100);
  tft_.print("(IN DEVELOPMENT)");

  tft_.drawFastHLine(20, 214, kScreenW - 40, kColorDim);
  tft_.setTextColor(kColorDim);
  tft_.setCursor(20, 222);
  tft_.print("BACK PAD1/7");
}

void UiScreens::showExport(ExportStatus status, uint8_t percent, uint16_t bpm,
                           uint32_t durationMs, uint32_t fileBytes, const char* fileName) {
  tft_.fillScreen(kColorBg);
  drawHeader("EXPORT", kColorOrange);

  tft_.setTextSize(2);
  tft_.setTextColor(kColorCream);
  tft_.setCursor(20, 60);
  tft_.print("PATTERN -> WAV");

  char info[48];
  snprintf(info, sizeof(info), "2 BARS x2  %u BPM  44.1 KHZ", (unsigned)bpm);
  tft_.setTextSize(1);
  tft_.setTextColor(kColorDim);
  tft_.setCursor(20, 90);
  tft_.print(info);
  snprintf(info, sizeof(info), "%u.%u SEC  %u KB", (unsigned)(durationMs / 1000),
           (unsigned)(durationMs % 1000 / 100), (unsigned)((fileBytes + 1023) / 1024));
  tft_.setCursor(20, 104);
  tft_.print(info);

  const char* statusText = "";
  uint16_t statusColor = kColorCream;
  switch (status) {
    case ExportStatus::Ready:
      statusText = "READY";
      break;
    case ExportStatus::Running:
      statusText = "SENDING TO SERIAL...";
      statusColor = kColorOrange;
      break;
    case ExportStatus::Done:
      statusText = "DONE";
      statusColor = kColorGreen;
      break;
    case ExportStatus::Aborted:
      statusText = "CANCELLED";
      statusColor = kColorMagenta;
      break;
  }
  tft_.setTextSize(2);
  tft_.setTextColor(statusColor);
  tft_.setCursor(20, 126);
  tft_.print(statusText);

  tft_.drawRect(kExportBarX - 2, kExportBarY - 2, kExportBarW + 4, kExportBarH + 4, kColorDim);
  updateExportProgress(status == ExportStatus::Ready ? 0 : percent);

  tft_.setTextSize(1);
  tft_.setTextColor(kColorDim);
  if (status == ExportStatus::Done) {
    tft_.setCursor(20, 180);
    tft_.print(fileName);
    tft_.setCursor(20, 194);
    tft_.print("SAVE WITH THE SMPLR EXPORT BOOKMARK");
  }

  tft_.drawFastHLine(20, 214, kScreenW - 40, kColorDim);
  tft_.setCursor(20, 222);
  tft_.print(status == ExportStatus::Running ? "CANCEL PAD7" : "START PAD5  BACK PAD1/7");
}

void UiScreens::updateExportProgress(uint8_t percent) {
  if (percent > 100) percent = 100;
  const int16_t fillW = (int16_t)((int32_t)kExportBarW * percent / 100);
  tft_.fillRect(kExportBarX, kExportBarY, fillW, kExportBarH, kColorOrange);
  tft_.fillRect(kExportBarX + fillW, kExportBarY, kExportBarW - fillW, kExportBarH, kColorBg);

  char pct[6];
  snprintf(pct, sizeof(pct), "%u%%", (unsigned)percent);
  tft_.fillRect(kScreenW - 20 - 24, 130, 24, 8, kColorBg);
  tft_.setTextSize(1);
  tft_.setTextColor(kColorCream);
  tft_.setCursor(kScreenW - 20 - (int16_t)strlen(pct) * 6, 130);
  tft_.print(pct);
}

void UiScreens::showSectionPage(uint8_t section) {
  tft_.fillScreen(kColorBg);
  char label[12];
  snprintf(label, sizeof(label), "SECTION %u", (unsigned)(section + 1));
  drawHeader(label, kColorOrange);

  tft_.setTextSize(2);
  tft_.setTextColor(kColorCream);
  tft_.setCursor(20, 70);
  tft_.print(label);

  tft_.setTextSize(1);
  tft_.setTextColor(kColorDim);
  tft_.setCursor(20, 100);
  tft_.print("(IN DEVELOPMENT)");

  tft_.drawFastHLine(20, 214, kScreenW - 40, kColorDim);
  tft_.setCursor(20, 222);
  tft_.print("BACK PAD7");
}

// ---------------------------------------------------------------------------
// Раздел 1: степ-секвенсор.
//
//  y=0   STEP SEQ                         > PLAY  120 BPM
//  y=40  ─────────────────────────────────────────────────
//  y=46       1                         2                  номера тактов
//  y=60       ▀                                            маркер бегущего шага
//  y=70  KICK □■□□ □□□□ ■□□□ □□□□   □□□□ ...               4 канала x 16 шагов
//  ...
//  y=210 ─────────────────────────────────────────────────
//  y=220 подсказка по управлению
//
// Ячейка 12x24 с шагом 16px; между тактами (после 8-го шага) лишние 6px.
// Курсор — двойная рамка в 2px вокруг ячейки, влезает в зазор между
// ячейками, поэтому перерисовка одной ячейки не задевает соседние.
namespace {
constexpr int16_t kSeqHeaderLineY = 40;
constexpr int16_t kSeqBarLabelY = 48;
constexpr int16_t kSeqMarkerY = 60;
constexpr int16_t kSeqMarkerH = 4;
constexpr int16_t kSeqGridX = 50;
constexpr int16_t kSeqGridY = 70;
constexpr int16_t kSeqCellW = 12;
constexpr int16_t kSeqCellH = 24;
constexpr int16_t kSeqStepPitch = 16;
constexpr int16_t kSeqRowPitch = 34;
constexpr int16_t kSeqBarGap = 6;
constexpr int16_t kSeqFooterLineY = 210;
constexpr int16_t kSeqTransportX = 170;

constexpr const char* kSeqTrackNames[StepSequencer::kTracks] = {"KICK", "SNARE", "HAT",
                                                                 "PERC"};
constexpr uint16_t kSeqTrackColors[StepSequencer::kTracks] = {kColorOrange, kColorCyan,
                                                              kColorLime, kColorMagenta};

int16_t seqStepX(uint8_t step) {
  return kSeqGridX + step * kSeqStepPitch +
         (step >= StepSequencer::kStepsPerBar ? kSeqBarGap : 0);
}

int16_t seqTrackY(uint8_t track) { return kSeqGridY + track * kSeqRowPitch; }
}  // namespace

void UiScreens::drawSequencerCell(const StepSequencer& seq, uint8_t track, uint8_t step,
                                  bool cursor) {
  const int16_t x = seqStepX(step);
  const int16_t y = seqTrackY(track);
  const bool on = seq.isOn(track, step);
  const bool playhead = step == seqPlayheadStep_;

  tft_.fillRect(x - 2, y - 2, kSeqCellW + 4, kSeqCellH + 4, kColorBg);
  if (on) {
    // Сработавший шаг под бегущей полосой вспыхивает кремовым.
    tft_.fillRect(x, y, kSeqCellW, kSeqCellH, playhead ? kColorCream : kSeqTrackColors[track]);
  } else if (playhead) {
    tft_.fillRect(x, y, kSeqCellW, kSeqCellH, kColorDim);
  } else {
    // Пустые ячейки на долях (каждая вторая восьмая) чуть заметнее — сетка
    // читается четвертями, как в Channel Rack.
    if (step % StepSequencer::kStepsPerBeat == 0) {
      tft_.fillRect(x + 1, y + 1, kSeqCellW - 2, kSeqCellH - 2, kColorGlitchDim);
    }
    tft_.drawRect(x, y, kSeqCellW, kSeqCellH, kColorDim);
  }

  if (cursor) {
    tft_.drawRect(x - 2, y - 2, kSeqCellW + 4, kSeqCellH + 4, kColorCream);
    tft_.drawRect(x - 1, y - 1, kSeqCellW + 2, kSeqCellH + 2, kColorCream);
  }
}

void UiScreens::drawSequencerLabel(uint8_t track, bool selected) {
  const int16_t y = seqTrackY(track);
  tft_.fillRect(16, y, kSeqGridX - 18, kSeqCellH, kColorBg);
  tft_.setTextSize(1);
  tft_.setTextColor(selected ? kColorCream : kColorDim);
  tft_.setCursor(16, y + 8);
  tft_.print(kSeqTrackNames[track]);
}

void UiScreens::drawSequencerPlayheadColumn(const StepSequencer& seq, uint8_t step, bool lit,
                                            uint8_t cursorTrack, uint8_t cursorStep) {
  const int16_t x = seqStepX(step);
  tft_.fillRect(x, kSeqMarkerY, kSeqCellW, kSeqMarkerH, lit ? kColorCream : kColorBg);
  for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
    drawSequencerCell(seq, t, step, t == cursorTrack && step == cursorStep);
    // Точка слева от названия канала горит, пока звучит его шаг.
    if (lit) {
      tft_.fillCircle(8, seqTrackY(t) + kSeqCellH / 2, 3,
                      seq.isOn(t, step) ? kSeqTrackColors[t] : kColorBg);
    }
  }
}

void UiScreens::showSequencer(const StepSequencer& seq, bool playing, uint8_t playhead,
                              uint8_t cursorTrack, uint8_t cursorStep, uint16_t bpm) {
  tft_.fillScreen(kColorBg);

  tft_.setTextSize(2);
  tft_.setTextColor(kColorCream);
  tft_.setCursor(12, 14);
  tft_.print("STEP SEQ");
  tft_.drawFastHLine(12, kSeqHeaderLineY, kScreenW - 24, kColorDim);
  updateSequencerTransport(playing, bpm);

  tft_.setTextSize(1);
  tft_.setTextColor(kColorDim);
  for (uint8_t bar = 0; bar < StepSequencer::kSteps / StepSequencer::kStepsPerBar; bar++) {
    tft_.setCursor(seqStepX(bar * StepSequencer::kStepsPerBar) + 3, kSeqBarLabelY);
    tft_.print(bar + 1);
  }

  seqPlayheadStep_ = playing ? playhead : 255;
  for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
    drawSequencerLabel(t, t == cursorTrack);
    for (uint8_t s = 0; s < StepSequencer::kSteps; s++) {
      drawSequencerCell(seq, t, s, t == cursorTrack && s == cursorStep);
    }
  }
  if (seqPlayheadStep_ != 255) {
    drawSequencerPlayheadColumn(seq, seqPlayheadStep_, true, cursorTrack, cursorStep);
  }

  tft_.drawFastHLine(12, kSeqFooterLineY, kScreenW - 24, kColorDim);
  tft_.setTextSize(1);
  tft_.setTextColor(kColorDim);
  tft_.setCursor(12, 220);
  tft_.print("PAD1/2/3/6 MOVE  PAD7 STEP  PLAY  PAD5 EXIT");
}

void UiScreens::updateSequencerCell(const StepSequencer& seq, uint8_t track, uint8_t step,
                                    uint8_t cursorTrack, uint8_t cursorStep) {
  drawSequencerCell(seq, track, step, track == cursorTrack && step == cursorStep);
}

void UiScreens::updateSequencerCursor(const StepSequencer& seq, uint8_t oldTrack,
                                      uint8_t oldStep, uint8_t cursorTrack,
                                      uint8_t cursorStep) {
  drawSequencerCell(seq, oldTrack, oldStep, false);
  drawSequencerCell(seq, cursorTrack, cursorStep, true);
  if (oldTrack != cursorTrack) {
    drawSequencerLabel(oldTrack, false);
    drawSequencerLabel(cursorTrack, true);
  }
}

void UiScreens::updateSequencerPlayhead(const StepSequencer& seq, uint8_t playhead,
                                        uint8_t cursorTrack, uint8_t cursorStep) {
  const uint8_t next = playhead;
  if (next == seqPlayheadStep_) return;

  const uint8_t prev = seqPlayheadStep_;
  seqPlayheadStep_ = next;
  if (prev != 255) {
    drawSequencerPlayheadColumn(seq, prev, false, cursorTrack, cursorStep);
  }
  if (next != 255) {
    drawSequencerPlayheadColumn(seq, next, true, cursorTrack, cursorStep);
  } else {
    for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
      tft_.fillCircle(8, seqTrackY(t) + kSeqCellH / 2, 3, kColorBg);
    }
  }
}

void UiScreens::updateSequencerTransport(bool playing, uint16_t bpm) {
  tft_.fillRect(kSeqTransportX, 10, kScreenW - 12 - kSeqTransportX, 24, kColorBg);

  const int16_t iconX = kSeqTransportX + 4;
  const int16_t iconY = 14;
  if (playing) {
    tft_.fillTriangle(iconX, iconY, iconX, iconY + 14, iconX + 12, iconY + 7, kColorGreen);
  } else {
    tft_.fillRect(iconX, iconY + 1, 12, 12, kColorDim);
  }

  tft_.setTextSize(1);
  tft_.setTextColor(playing ? kColorGreen : kColorDim);
  tft_.setCursor(iconX + 20, iconY + 4);
  tft_.print(playing ? "PLAY" : "STOP");

  char bpmStr[10];
  snprintf(bpmStr, sizeof(bpmStr), "%u BPM", bpm);
  tft_.setTextColor(kColorCream);
  tft_.setCursor(kScreenW - 12 - (int16_t)strlen(bpmStr) * 6, iconY + 4);
  tft_.print(bpmStr);
}
