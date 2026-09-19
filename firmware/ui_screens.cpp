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

// Альбомная ориентация: setRotation(1) в begin() даёт width()=320,
// height()=240 — см. также "rotate" у lcd1 в diagram.json (должен
// физически развернуть деталь на экране симуляции в ту же сторону).
using uicolor::kScreenW;
using uicolor::kScreenH;

constexpr uint16_t kColorBg = uicolor::kBg;
constexpr uint16_t kColorCream = uicolor::kCream;
constexpr uint16_t kColorOrange = uicolor::kOrange;
constexpr uint16_t kColorGreen = uicolor::kGreen;
constexpr uint16_t kColorDim = uicolor::kDim;
constexpr uint16_t kColorHint = uicolor::kHint;
constexpr uint16_t kColorMagenta = uicolor::kMagenta;
constexpr uint16_t kColorCyan = uicolor::kCyan;
constexpr uint16_t kColorLime = uicolor::kLime;
constexpr uint16_t kColorGlitchDim = uicolor::kGlitchDim;
// CYCLE = [mag, cyan, lime, yel] — порядок цветов прогресс-бара и конфетти.
constexpr uint16_t kGlitchPalette[4] = {uicolor::kMagenta, uicolor::kCyan, uicolor::kLime,
                                         uicolor::kYellow};

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

// Подвал страниц: линия и две строки подсказки.
constexpr int16_t kPageFooterLineY = 204;
constexpr int16_t kPageHint1Y = 211;
constexpr int16_t kPageHint2Y = 223;

// Главный экран (B.2 + метроном): слева BPM/метроном/разделы проекта в колонке
// шириной kHomeLeftW, справа — вертикальный индикатор уровня сигнала во всю
// высоту рабочей области экрана.
constexpr int16_t kHomeLeftW = 220;
constexpr int16_t kMeterX = 244;
constexpr int16_t kMeterW = 56;
constexpr int16_t kMeterY = 54;
constexpr int16_t kMeterBottom = 210;
constexpr int16_t kMeterH = kMeterBottom - kMeterY;

// Полоса удержания POWER на экране "выключено".
constexpr int16_t kPowerBarX = 60;
constexpr int16_t kPowerBarY = 214;
constexpr int16_t kPowerBarW = 200;
constexpr int16_t kPowerBarH = 4;
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
  screen_ = Screen::Off;
  bootDrawn_ = false;  // следующий showBoot() снова нарисует экран с нуля
  lastPowerHoldPx_ = 0;
  // Устройство "выключено" мягко: экран жив, поэтому подсказываем, как
  // включить — иначе чёрный экран не отличить от зависшей симуляции.
  const char* hint = "HOLD POWER TO TURN ON";
  drawText((kScreenW - (int16_t)strlen(hint) * 6) / 2, 196, hint, 1, kColorHint, ILI9341_BLACK);
}

void UiScreens::updatePowerHold(uint8_t percent) {
  if (screen_ != Screen::Off) return;
  if (percent > 100) percent = 100;
  const uint8_t px = (uint8_t)((uint32_t)kPowerBarW * percent / 100);
  if (px == lastPowerHoldPx_) return;
  if (px > lastPowerHoldPx_) {
    tft_.fillRect(kPowerBarX + lastPowerHoldPx_, kPowerBarY, px - lastPowerHoldPx_, kPowerBarH,
                  kColorOrange);
  } else {
    tft_.fillRect(kPowerBarX + px, kPowerBarY, lastPowerHoldPx_ - px, kPowerBarH, ILI9341_BLACK);
  }
  lastPowerHoldPx_ = px;
}

uint8_t UiScreens::nextRand() {
  // xorshift32 — детерминированный ГПСЧ, достаточно "случайный" на вид для
  // разлёта глитч-пикселей, не требует истинной энтропии.
  rngState_ ^= rngState_ << 13;
  rngState_ ^= rngState_ >> 17;
  rngState_ ^= rngState_ << 5;
  return (uint8_t)(rngState_ & 0xFF);
}

// Adafruit_GFX рисует прозрачный текст точка за точкой, и на каждую точку
// открывает своё окно адресов: CASET/PASET/RAMWR, три переключения DC и
// отдельные SPI-обмены. Строка подсказки из 26 символов обходилась так в
// тысячи SPI-транзакций — дороже заливки всего экрана. Здесь строка
// рисуется в канвас в RAM (процессор, без SPI), а на экран уходит одним
// окном — по одному вызову writePixels на строку пикселей.
void UiScreens::drawText(int16_t x, int16_t y, const char* text, uint8_t size, uint16_t fg,
                         uint16_t bg) {
  const size_t len = strlen(text);
  if (len == 0) return;
  const int16_t w = (int16_t)(len * 6 * size);  // 5x7 + межбуквенный столбец
  const int16_t h = (int16_t)(8 * size);
  GFXcanvas16 canvas(w, h);
  if (canvas.getBuffer() == nullptr) {
    // Не хватило памяти под канвас — рисуем по-старому, медленно, но верно.
    tft_.setTextSize(size);
    tft_.setTextColor(fg);
    tft_.setCursor(x, y);
    tft_.print(text);
    return;
  }
  canvas.fillScreen(bg);
  canvas.setTextWrap(false);
  canvas.setTextSize(size);
  canvas.setTextColor(fg);
  canvas.setCursor(0, 0);
  canvas.print(text);
  if (x >= 0 && y >= 0 && x + w <= kScreenW && y + h <= kScreenH) {
    // Весь канвас — одним вызовом: drawRGBBitmap() шлёт его построчно.
    tft_.startWrite();
    tft_.setAddrWindow(x, y, w, h);
    tft_.writePixels(canvas.getBuffer(), (uint32_t)w * h);
    tft_.endWrite();
  } else {
    tft_.drawRGBBitmap(x, y, canvas.getBuffer(), w, h);  // обрежет по краю экрана
  }
}

void UiScreens::drawTextField(int16_t x, int16_t y, int16_t w, const char* text, uint8_t size,
                              uint16_t fg, uint16_t bg) {
  tft_.fillRect(x, y, w, 8 * size, bg);
  drawText(x, y, text, size, fg, bg);
}

void UiScreens::drawHeader(const char* rightLabel, uint16_t rightColor) {
  drawText(20, 24, "SMPLR", 2, kColorCream, kColorBg);

  int16_t textW = (int16_t)strlen(rightLabel) * 6;  // 5x7 font, size 1
  drawText(kScreenW - 20 - textW, 30, rightLabel, 1, rightColor, kColorBg);

  tft_.drawFastHLine(20, 46, kScreenW - 40, kColorDim);
}

void UiScreens::drawBootFooter() {
  drawText(21, kFooterY, "PRERELEASE V" SMPLR_VERSION, 1, kColorMagenta, kColorBg);

  const char* right = "SMPLR OS";
  const int16_t textW = (int16_t)strlen(right) * 6;
  drawText(kScreenW - 21 - textW, kFooterY, right, 1, kColorCyan, kColorBg);
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

  const uint8_t dots = (uint8_t)((millis() / 400) % 4);
  char status[12] = "BOOTING";
  for (uint8_t i = 0; i < dots; i++) strcat(status, ".");
  drawText(21, kStatusY, status, 1, kColorLime, kColorBg);

  char pctStr[6];
  snprintf(pctStr, sizeof(pctStr), "%u%%", percent);
  const int16_t textW = (int16_t)strlen(pctStr) * 6;  // 6px*size1/char
  drawText(kScreenW - 21 - textW, kStatusY, pctStr, 1, kColorCream, kColorBg);
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
    screen_ = Screen::Other;
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


// ---------------------------------------------------------------------------
// Главный экран.
namespace {
// Геометрия главного экрана — общая для полной и частичной перерисовки.
constexpr int16_t kHomeBpmX = 28;
constexpr int16_t kHomeBpmLabelY = 104;
constexpr int16_t kHomeMetX = 152;
constexpr int16_t kHomeMetY = 51;
constexpr int16_t kHomeMetW = 56;
constexpr int16_t kHomeMetH = 46;
constexpr int16_t kHomeRowY = 136;
constexpr int16_t kHomeBoxW = 40;
constexpr int16_t kHomeBoxGap = 6;
constexpr int16_t kHomeBoxStartX = (kHomeLeftW - (4 * kHomeBoxW + 3 * kHomeBoxGap)) / 2;
constexpr int16_t kHomeSectionLabelY = kHomeRowY + 38;
constexpr int16_t kHomeHintLineY = 188;
constexpr int16_t kHomeStatusRight = kScreenW - 20;
constexpr int16_t kHomeStatusW = 96;
constexpr int16_t kHomeExportX = 100;
constexpr int16_t kHomeExportW = 84;
constexpr uint8_t kNoSectionCursor = 255;

constexpr const char* kSectionNames[kSectionCount] = {"SEQ", "MIX", "ROLL", "ARR"};
}  // namespace

// Каждый элемент главного экрана сам стирает свою область — так его можно
// перерисовать отдельно, не трогая остальной экран.
void UiScreens::drawHomeBpm(uint16_t bpm, bool focused) {
  // Область рамки фокуса: от kHomeBpmX - 10, шириной под три цифры + 20.
  tft_.fillRect(kHomeBpmX - 10, 51, 92, 46, kColorBg);

  char bpmStr[6];
  snprintf(bpmStr, sizeof(bpmStr), "%u", bpm);
  const int16_t bpmTextW = (int16_t)strlen(bpmStr) * 24;  // 6px*size4/char
  drawText(kHomeBpmX, 58, bpmStr, 4, kColorCream, kColorBg);

  if (focused) {
    const int16_t frameW = max(bpmTextW, (int16_t)48) + 20;
    tft_.drawRoundRect(kHomeBpmX - 10, 51, frameW, 46, 6, kColorOrange);
    tft_.drawRoundRect(kHomeBpmX - 9, 52, frameW - 2, 44, 5, kColorOrange);
  }

  // Подпись под темпом говорит, чем его крутить: K1 меняет темп всегда,
  // курсор на BPM только подсвечивает подсказку.
  tft_.fillRect(kHomeBpmX, kHomeBpmLabelY, 90, 8, kColorBg);
  drawText(kHomeBpmX, kHomeBpmLabelY, "BPM", 1, kColorOrange, kColorBg);
  drawText(kHomeBpmX + 24, kHomeBpmLabelY, focused ? "TURN K1" : "K1", 1,
           focused ? kColorCream : kColorHint, kColorBg);
}

// Кнопка метронома — справа от темпа, в той же строке.
void UiScreens::drawHomeMetronome(bool on, bool focused) {
  tft_.fillRect(kHomeMetX - 3, kHomeMetY - 3, kHomeMetW + 6, kHomeMetH + 6, kColorBg);
  uint16_t fg;
  uint16_t bg;
  if (on) {
    tft_.fillRoundRect(kHomeMetX, kHomeMetY, kHomeMetW, kHomeMetH, 6, kColorGreen);
    fg = kColorBg;
    bg = kColorGreen;
  } else {
    tft_.drawRoundRect(kHomeMetX, kHomeMetY, kHomeMetW, kHomeMetH, 6, kColorDim);
    fg = kColorHint;
    bg = kColorBg;
  }
  drawText(kHomeMetX + 9, kHomeMetY + 12, "MET", 1, fg, bg);
  drawText(kHomeMetX + 9, kHomeMetY + 26, on ? "ON" : "OFF", 1, fg, bg);
  if (focused) {
    tft_.drawRoundRect(kHomeMetX - 3, kHomeMetY - 3, kHomeMetW + 6, kHomeMetH + 6, 8,
                       kColorOrange);
  }
}

void UiScreens::drawHomeSection(uint8_t section, bool active, bool cursor) {
  const int16_t x = kHomeBoxStartX + section * (kHomeBoxW + kHomeBoxGap);
  tft_.fillRect(x - 3, kHomeRowY - 3, kHomeBoxW + 6, 38, kColorBg);
  uint16_t fg;
  uint16_t bg;
  if (active) {
    tft_.fillRoundRect(x, kHomeRowY, kHomeBoxW, 32, 5, kColorOrange);
    fg = kColorBg;
    bg = kColorOrange;
  } else {
    tft_.drawRoundRect(x, kHomeRowY, kHomeBoxW, 32, 5, kColorDim);
    fg = kColorHint;
    bg = kColorBg;
  }
  const char label[2] = {(char)('1' + section), '\0'};
  drawText(x + kHomeBoxW / 2 - 6, kHomeRowY + 8, label, 2, fg, bg);
  if (cursor) {
    tft_.drawRoundRect(x - 3, kHomeRowY - 3, kHomeBoxW + 6, 38, 7, kColorCream);
  }

  // Имя раздела под кнопкой — чтобы было видно, что внутри, до открытия.
  const char* name = kSectionNames[section];
  const int16_t nameW = (int16_t)strlen(name) * 6;
  tft_.fillRect(x - 3, kHomeSectionLabelY, kHomeBoxW + 6, 8, kColorBg);
  drawText(x + (kHomeBoxW - nameW) / 2, kHomeSectionLabelY, name, 1,
           active ? kColorOrange : kColorHint, kColorBg);
}

// Статус транспорта в шапке: READY, PLAY с номером такта или PLAY EMPTY,
// если играть нечего (паттерн пуст, метроном выключен) — иначе PLAY на
// пустом паттерне выглядит как поломка: надпись есть, звука нет.
void UiScreens::drawHomeStatus(const HomeView& v) {
  char label[16];
  if (!v.playing) {
    snprintf(label, sizeof(label), "READY");
  } else if (v.silent) {
    snprintf(label, sizeof(label), "PLAY EMPTY");
  } else {
    snprintf(label, sizeof(label), "PLAY BAR %u/4", (unsigned)(v.bar + 1));
  }
  if (strcmp(label, homeStatus_) == 0) return;
  strncpy(homeStatus_, label, sizeof(homeStatus_) - 1);

  const int16_t textW = (int16_t)strlen(label) * 6;
  tft_.fillRect(kHomeStatusRight - kHomeStatusW, 26, kHomeStatusW, 12, kColorBg);
  const uint16_t color = v.playing && v.silent ? kColorHint : kColorGreen;
  drawText(kHomeStatusRight - textW, 30, label, 1, color, kColorBg);
  if (v.playing) {
    const int16_t tx = kHomeStatusRight - textW - 12;
    tft_.fillTriangle(tx, 29, tx, 37, tx + 7, 33, color);
  }
}

// Экспорт идёт в фоне, пока пользователь на других экранах: без этой
// отметки он не знает ни что экспорт ещё идёт, ни что он уже закончился.
void UiScreens::drawHomeExportBadge(const HomeView& v) {
  char label[16] = "";
  uint16_t color = kColorOrange;
  if (v.exportStatus == ExportStatus::Running) {
    snprintf(label, sizeof(label), "EXPORT %u%%", (unsigned)v.exportPercent);
  } else if (v.exportUnseen && v.exportStatus == ExportStatus::Done) {
    snprintf(label, sizeof(label), "EXPORT DONE");
    color = kColorGreen;
  }
  if (strcmp(label, homeExport_) == 0) return;
  strncpy(homeExport_, label, sizeof(homeExport_) - 1);

  tft_.fillRect(kHomeExportX, 26, kHomeExportW, 12, kColorBg);
  if (label[0] != '\0') drawText(kHomeExportX, 30, label, 1, color, kColorBg);
}

void UiScreens::showHome(const HomeView& v) {
  // Рамка курсора на разделе видна, только пока фокус в ряду разделов.
  const uint8_t cursor = v.focus == HomeFocus::Sections ? v.sectionCursor : kNoSectionCursor;

  if (screen_ == Screen::Home) {
    // Экран уже нарисован — только изменившиеся элементы.
    if (v.bpm != home_.bpm || (v.focus == HomeFocus::Bpm) != (home_.focus == HomeFocus::Bpm)) {
      drawHomeBpm(v.bpm, v.focus == HomeFocus::Bpm);
    }
    if (v.metronomeOn != home_.metronomeOn ||
        (v.focus == HomeFocus::Metronome) != (home_.focus == HomeFocus::Metronome)) {
      drawHomeMetronome(v.metronomeOn, v.focus == HomeFocus::Metronome);
    }
    const uint8_t oldCursor =
        home_.focus == HomeFocus::Sections ? home_.sectionCursor : kNoSectionCursor;
    for (uint8_t i = 0; i < kSectionCount; i++) {
      const bool activeChanged = (i == home_.activeSection) != (i == v.activeSection);
      const bool cursorChanged = (i == oldCursor) != (i == cursor);
      if (activeChanged || cursorChanged) {
        drawHomeSection(i, i == v.activeSection, i == cursor);
      }
    }
    drawHomeStatus(v);
    drawHomeExportBadge(v);
  } else {
    tft_.fillScreen(kColorBg);
    screen_ = Screen::Home;
    homeStatus_[0] = '\0';
    homeExport_[0] = '\0';
    drawHeader("", kColorGreen);
    drawHomeStatus(v);
    drawHomeExportBadge(v);

    drawHomeBpm(v.bpm, v.focus == HomeFocus::Bpm);
    drawHomeMetronome(v.metronomeOn, v.focus == HomeFocus::Metronome);
    drawText(kHomeMetX + 9, kHomeBpmLabelY, "PAD4", 1, kColorHint, kColorBg);
    for (uint8_t i = 0; i < kSectionCount; i++) {
      drawHomeSection(i, i == v.activeSection, i == cursor);
    }

    tft_.drawFastHLine(20, kHomeHintLineY, kHomeLeftW - 20, kColorDim);
    drawText(20, kHomeHintLineY + 6, "PAD1/2/3/6 MOVE  PAD5 SELECT,", 1, kColorHint, kColorBg);
    drawText(20, kHomeHintLineY + 16, "AGAIN TO OPEN  MODE MENU", 1, kColorHint, kColorBg);

    // Рамка индикатора уровня сигнала — статичная часть; саму заливку по
    // кадрам рисует updateSoundMeter(), чтобы не перерисовывать весь экран.
    tft_.drawRoundRect(kMeterX - 4, kMeterY - 4, kMeterW + 8, kMeterH + 8, 4, kColorDim);
    drawText(kMeterX + (kMeterW - 18) / 2, kMeterBottom + 8, "OUT", 1, kColorHint, kColorBg);
    lastMeterFillPx_ = 255;
    updateSoundMeter(0.0f);
  }

  home_ = v;
}

void UiScreens::updateSoundMeter(float level) {
  if (screen_ != Screen::Home) return;
  if (level < 0.0f) level = 0.0f;
  if (level > 1.0f) level = 1.0f;

  const uint8_t fillPx = (uint8_t)(level * kMeterH + 0.5f);
  if (fillPx == lastMeterFillPx_) return;

  // Перерисовываем только полоску между старым и новым уровнем. Вызов идёт
  // каждые 30 мс, и перерисовка всей полосы на каждом кадре спада стоила
  // по SPI больше десятой части полного экрана.
  if (lastMeterFillPx_ == 255) {
    tft_.fillRect(kMeterX, kMeterY, kMeterW, kMeterH - fillPx, kColorBg);
    if (fillPx > 0) {
      tft_.fillRect(kMeterX, kMeterY + (kMeterH - fillPx), kMeterW, fillPx, kColorOrange);
    }
  } else if (fillPx > lastMeterFillPx_) {
    tft_.fillRect(kMeterX, kMeterY + (kMeterH - fillPx), kMeterW, fillPx - lastMeterFillPx_,
                  kColorOrange);
  } else {
    tft_.fillRect(kMeterX, kMeterY + (kMeterH - lastMeterFillPx_), kMeterW,
                  lastMeterFillPx_ - fillPx, kColorBg);
  }
  lastMeterFillPx_ = fillPx;
}

// ---------------------------------------------------------------------------
// Меню.
namespace {
constexpr const char* kMenuItems[kMenuItemCount] = {"TRACK", "TEMPO", "INPUT", "SYSTEM",
                                                    "EXPORT"};

constexpr int16_t kExportBarX = 20;
constexpr int16_t kExportBarY = 148;
constexpr int16_t kExportBarW = kScreenW - 40;
constexpr int16_t kExportBarH = 14;

constexpr int16_t kTempoRowX = 20;
constexpr int16_t kTempoRowW = kScreenW - 40;
constexpr int16_t kTempoRowH = 34;
constexpr int16_t kTempoRowY[2] = {64, 110};
}  // namespace

// Общий каркас полноэкранной страницы: шапка с названием и две строки
// подсказки внизу.
void UiScreens::drawPageFrame(const char* title, const char* hint1, const char* hint2) {
  tft_.fillScreen(kColorBg);
  screen_ = Screen::Other;
  drawHeader(title, kColorOrange);
  tft_.drawFastHLine(20, kPageFooterLineY, kScreenW - 40, kColorDim);
  if (hint1 != nullptr) drawText(20, kPageHint1Y, hint1, 1, kColorHint, kColorBg);
  if (hint2 != nullptr) drawText(20, kPageHint2Y, hint2, 1, kColorHint, kColorBg);
}

void UiScreens::drawMenuRow(uint8_t item, bool selected) {
  const int16_t rowH = 28;
  const int16_t y = 54 + item * rowH;
  uint16_t fg;
  uint16_t bg;
  if (selected) {
    tft_.fillRoundRect(20, y, kScreenW - 40, rowH - 6, 5, kColorOrange);
    fg = kColorBg;
    bg = kColorOrange;
  } else {
    tft_.fillRect(20, y, kScreenW - 40, rowH - 6, kColorBg);
    tft_.drawRoundRect(20, y, kScreenW - 40, rowH - 6, 5, kColorDim);
    fg = kColorCream;
    bg = kColorBg;
  }
  drawText(32, y + 4, kMenuItems[item], 2, fg, bg);
}

void UiScreens::showMenuList(uint8_t selected) {
  if (screen_ == Screen::MenuList) {
    // Список уже на экране — перерисовать только бывшую и новую строку.
    if (selected != menuSelected_) {
      drawMenuRow(menuSelected_, false);
      drawMenuRow(selected, true);
      menuSelected_ = selected;
    }
    return;
  }

  drawPageFrame("MENU", "PAD6/2 MOVE  PAD5/PAD3 OPEN", "PAD7/PAD1 OR MODE BACK");
  screen_ = Screen::MenuList;
  for (uint8_t i = 0; i < kMenuItemCount; i++) {
    drawMenuRow(i, i == selected);
  }
  menuSelected_ = selected;
}

void UiScreens::showMenuStub(uint8_t itemIndex) {
  drawPageFrame(kMenuItems[itemIndex], "PAD7 BACK", nullptr);
  drawText(20, 64, "COMING SOON", 2, kColorCream, kColorBg);
  if (itemIndex == kMenuItemTrack) {
    drawText(20, 96, "SAMPLE PER CHANNEL - NEEDS SD CARD", 1, kColorHint, kColorBg);
    drawText(20, 110, "CHANNEL VOLUME AND MUTE ARE IN", 1, kColorHint, kColorBg);
    drawText(20, 122, "SECTION 2 - MIX", 1, kColorHint, kColorBg);
  }
}

void UiScreens::drawTempoRow(uint8_t row, uint16_t bpm, bool metronomeOn, bool selected) {
  const int16_t y = kTempoRowY[row];
  tft_.fillRect(kTempoRowX, y, kTempoRowW, kTempoRowH, kColorBg);
  tft_.drawRoundRect(kTempoRowX, y, kTempoRowW, kTempoRowH, 5,
                     selected ? kColorOrange : kColorDim);
  if (selected) {
    tft_.drawRoundRect(kTempoRowX + 1, y + 1, kTempoRowW - 2, kTempoRowH - 2, 4, kColorOrange);
  }
  char value[8];
  uint16_t valueColor = kColorCream;
  if (row == 0) {
    drawText(kTempoRowX + 12, y + 13, "BPM", 1, kColorHint, kColorBg);
    snprintf(value, sizeof(value), "%u", bpm);
  } else {
    drawText(kTempoRowX + 12, y + 13, "METRONOME", 1, kColorHint, kColorBg);
    snprintf(value, sizeof(value), "%s", metronomeOn ? "ON" : "OFF");
    valueColor = metronomeOn ? kColorGreen : kColorHint;
  }
  const int16_t w = (int16_t)strlen(value) * 12;
  drawText(kTempoRowX + kTempoRowW - 16 - w, y + 10, value, 2, valueColor, kColorBg);
}

void UiScreens::showTempo(uint16_t bpm, bool metronomeOn, uint8_t row) {
  if (screen_ != Screen::Tempo) {
    drawPageFrame("TEMPO", "PAD6/2 ROW  PAD1/3 -/+  K1 BPM", "PAD5 MET ON/OFF  PAD7 BACK");
    screen_ = Screen::Tempo;
    drawTempoRow(0, bpm, metronomeOn, row == 0);
    drawTempoRow(1, bpm, metronomeOn, row == 1);
    drawText(20, 160, "METRONOME CLICKS ONLY WHILE PLAYING,", 1, kColorHint, kColorBg);
    drawText(20, 172, "ON EVERY BEAT, ACCENT ON BAR START", 1, kColorHint, kColorBg);
  } else {
    if (bpm != tempoBpm_ || (row == 0) != (tempoRow_ == 0)) {
      drawTempoRow(0, bpm, metronomeOn, row == 0);
    }
    if (metronomeOn != tempoMet_ || (row == 1) != (tempoRow_ == 1)) {
      drawTempoRow(1, bpm, metronomeOn, row == 1);
    }
  }
  tempoBpm_ = bpm;
  tempoMet_ = metronomeOn;
  tempoRow_ = row;
}

void UiScreens::drawInputValues(uint8_t knobSpeed, uint8_t lastKnob, int8_t lastDelta) {
  char buf[24];
  snprintf(buf, sizeof(buf), "x%u", (unsigned)knobSpeed);
  drawTextField(200, 62, 100, buf, 2, kColorCream, kColorBg);

  if (lastKnob == 255) {
    snprintf(buf, sizeof(buf), "-");
  } else {
    snprintf(buf, sizeof(buf), "K%u %+d", (unsigned)(lastKnob + 1), (int)lastDelta);
  }
  drawTextField(200, 136, 100, buf, 2, kColorCream, kColorBg);
}

void UiScreens::showInput(uint8_t knobSpeed, uint8_t lastKnob, int8_t lastDelta) {
  if (screen_ != Screen::Input) {
    drawPageFrame("INPUT", "PAD1/3 KNOB SPEED -/+", "TURN ANY KNOB TO TEST  PAD7 BACK");
    screen_ = Screen::Input;
    drawText(20, 66, "KNOB SPEED", 1, kColorHint, kColorBg);
    drawText(20, 84, "HOW MUCH ONE KNOB CLICK CHANGES", 1, kColorHint, kColorBg);
    drawText(20, 96, "A VALUE (BPM, VOLUME)", 1, kColorHint, kColorBg);
    drawText(20, 140, "LAST KNOB", 1, kColorHint, kColorBg);
    drawText(20, 166, "SOURCE  WOKWI VIRTUAL MPK", 1, kColorHint, kColorBg);
    drawText(20, 178, "KNOBS ARE ENDLESS: VALUES NEVER JUMP", 1, kColorHint, kColorBg);
    drawInputValues(knobSpeed, lastKnob, lastDelta);
  } else if (knobSpeed != inputSpeed_ || lastKnob != inputKnob_ || lastDelta != inputDelta_) {
    drawInputValues(knobSpeed, lastKnob, lastDelta);
  }
  inputSpeed_ = knobSpeed;
  inputKnob_ = lastKnob;
  inputDelta_ = lastDelta;
}

void UiScreens::showSystem(const SystemInfo& info) {
  drawPageFrame("SYSTEM", "PAD7 BACK", nullptr);
  drawText(20, 60, "SMPLR OS", 2, kColorCream, kColorBg);
  drawText(20, 82, "PRERELEASE V" SMPLR_VERSION, 1, kColorMagenta, kColorBg);
  char buf[48];
  snprintf(buf, sizeof(buf), "BUILD  %s %s", __DATE__, __TIME__);
  drawText(20, 104, buf, 1, kColorHint, kColorBg);
  snprintf(buf, sizeof(buf), "FLASH  %lu KB   PSRAM  %lu KB", (unsigned long)info.flashKb,
           (unsigned long)info.psramKb);
  drawText(20, 120, buf, 1, kColorHint, kColorBg);
  snprintf(buf, sizeof(buf), "FREE RAM  %lu KB", (unsigned long)info.heapFreeKb);
  drawText(20, 136, buf, 1, kColorHint, kColorBg);
  snprintf(buf, sizeof(buf), "UPTIME  %lu S", (unsigned long)info.uptimeS);
  drawText(20, 152, buf, 1, kColorHint, kColorBg);
}

void UiScreens::showExport(const ExportView& v) {
  const char* hint1 = "PAD5 START  PAD7 BACK";
  if (v.status == ExportStatus::Running) {
    hint1 = "PAD7 CANCEL";
  } else if (v.status == ExportStatus::Done || v.status == ExportStatus::Aborted) {
    hint1 = "PAD5 EXPORT AGAIN  PAD7 BACK";
  }
  drawPageFrame("EXPORT", hint1,
                v.status == ExportStatus::Running ? "MODE LEAVE - EXPORT KEEPS GOING" : nullptr);

  drawText(20, 56, "PATTERN -> WAV", 2, kColorCream, kColorBg);

  char info[48];
  snprintf(info, sizeof(info), "4 BARS  %u BPM  44.1 KHZ  MONO", (unsigned)v.bpm);
  drawText(20, 82, info, 1, kColorHint, kColorBg);
  snprintf(info, sizeof(info), "%u.%u SEC  %u KB  MIXER LEVELS APPLY", (unsigned)(v.durationMs / 1000),
           (unsigned)(v.durationMs % 1000 / 100), (unsigned)((v.fileBytes + 1023) / 1024));
  drawText(20, 96, info, 1, kColorHint, kColorBg);

  const char* statusText = "";
  uint16_t statusColor = kColorCream;
  switch (v.status) {
    case ExportStatus::Ready:
      statusText = "READY";
      break;
    case ExportStatus::Running:
      statusText = "SENDING...";
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
    case ExportStatus::Empty:
      statusText = "PATTERN IS EMPTY";
      statusColor = kColorMagenta;
      break;
  }
  drawText(20, 120, statusText, 2, statusColor, kColorBg);

  tft_.drawRect(kExportBarX - 2, kExportBarY - 2, kExportBarW + 4, kExportBarH + 4, kColorDim);
  const bool showProgress = v.status == ExportStatus::Running || v.status == ExportStatus::Done;
  updateExportProgress(showProgress ? v.percent : 0);

  switch (v.status) {
    case ExportStatus::Running:
      drawText(20, 172, "PLAY/STOP IS OFF WHILE EXPORTING", 1, kColorHint, kColorBg);
      break;
    case ExportStatus::Done:
      drawText(20, 172, v.fileName, 1, kColorCream, kColorBg);
      drawText(20, 186, "SAVE WITH THE SMPLR EXPORT BOOKMARK", 1, kColorHint, kColorBg);
      break;
    case ExportStatus::Aborted:
      drawText(20, 172, "NO FILE WAS SAVED", 1, kColorHint, kColorBg);
      break;
    case ExportStatus::Empty:
      drawText(20, 172, "DRAW SOME STEPS IN SECTION 1 (SEQ)", 1, kColorHint, kColorBg);
      break;
    default:
      drawText(20, 172, "PLAYBACK STOPS WHILE EXPORTING", 1, kColorHint, kColorBg);
      break;
  }
}

void UiScreens::updateExportProgress(uint8_t percent) {
  if (percent > 100) percent = 100;
  const int16_t fillW = (int16_t)((int32_t)kExportBarW * percent / 100);
  tft_.fillRect(kExportBarX, kExportBarY, fillW, kExportBarH, kColorOrange);
  tft_.fillRect(kExportBarX + fillW, kExportBarY, kExportBarW - fillW, kExportBarH, kColorBg);

  char pct[6];
  snprintf(pct, sizeof(pct), "%u%%", (unsigned)percent);
  drawTextField(kScreenW - 20 - 24, 126, 24, "", 1, kColorCream, kColorBg);
  drawText(kScreenW - 20 - (int16_t)strlen(pct) * 6, 126, pct, 1, kColorCream, kColorBg);
}

// ---------------------------------------------------------------------------
// Раздел 1: степ-секвенсор.
//
//  y=0   STEP SEQ          > PLAY   MET         120 BPM
//  y=40  ─────────────────────────────────────────────────
//  y=48      1         2         3         4             номера тактов
//  y=60      ▀                                           маркер бегущего шага
//  y=70  KICK □■□□ □□□□ ■□□□ □□□□                        4 канала x 16 шагов
//  ...
//  y=204 ─────────────────────────────────────────────────
//  y=211 подсказка по управлению, две строки
//
// Шаг — четверть, 4 шага на такт, 4 такта. Ячейка 11x24 с шагом 15px, между
// тактами лишние 5px. Курсор — двойная рамка в 2px вокруг ячейки, влезает в
// зазор между ячейками, поэтому перерисовка одной ячейки не задевает соседние.
namespace {
constexpr int16_t kSeqHeaderLineY = 40;
constexpr int16_t kSeqBarLabelY = 48;
constexpr int16_t kSeqMarkerY = 60;
constexpr int16_t kSeqMarkerH = 4;
constexpr int16_t kSeqGridX = 50;
constexpr int16_t kSeqGridY = 70;
constexpr int16_t kSeqCellW = 11;
constexpr int16_t kSeqCellH = 24;
constexpr int16_t kSeqStepPitch = 15;
constexpr int16_t kSeqRowPitch = 34;
constexpr int16_t kSeqBarGap = 5;
constexpr int16_t kSeqTransportX = 136;
constexpr int16_t kSeqMetX = 200;

constexpr const char* kSeqTrackNames[StepSequencer::kTracks] = {"KICK", "SNARE", "HAT",
                                                                 "PERC"};

int16_t seqStepX(uint8_t step) {
  return kSeqGridX + step * kSeqStepPitch + (step / StepSequencer::kStepsPerBar) * kSeqBarGap;
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
    tft_.fillRect(x, y, kSeqCellW, kSeqCellH, playhead ? kColorCream : uicolor::kTrack[track]);
  } else if (playhead) {
    tft_.fillRect(x, y, kSeqCellW, kSeqCellH, kColorDim);
  } else {
    // Первая доля такта у пустых ячеек чуть заметнее — сетка читается
    // тактами, как в Channel Rack.
    if (step % StepSequencer::kStepsPerBar == 0) {
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
  drawText(16, y + 8, kSeqTrackNames[track], 1, selected ? kColorCream : kColorHint, kColorBg);
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
                      seq.isOn(t, step) ? uicolor::kTrack[t] : kColorBg);
    }
  }
}

void UiScreens::showSequencer(const StepSequencer& seq, bool playing, uint8_t playhead,
                              uint8_t cursorTrack, uint8_t cursorStep, uint16_t bpm,
                              bool metronomeOn) {
  tft_.fillScreen(kColorBg);
  screen_ = Screen::Other;

  drawText(12, 14, "STEP SEQ", 2, kColorCream, kColorBg);
  tft_.drawFastHLine(12, kSeqHeaderLineY, kScreenW - 24, kColorDim);
  updateSequencerHeader(playing, bpm, metronomeOn);

  for (uint8_t bar = 0; bar < StepSequencer::kSteps / StepSequencer::kStepsPerBar; bar++) {
    const char label[2] = {(char)('1' + bar), '\0'};
    drawText(seqStepX(bar * StepSequencer::kStepsPerBar) + 3, kSeqBarLabelY, label, 1, kColorHint,
             kColorBg);
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

  tft_.drawFastHLine(12, kPageFooterLineY, kScreenW - 24, kColorDim);
  drawText(12, kPageHint1Y, "PAD1/2/3/6 MOVE  PAD5 STEP  PAD7 BACK", 1, kColorHint, kColorBg);
  updateSequencerFooter(255);
}

void UiScreens::updateSequencerFooter(uint8_t confirmTrack) {
  char text[40];
  uint16_t color = kColorHint;
  if (confirmTrack < StepSequencer::kTracks) {
    snprintf(text, sizeof(text), "PAD8 AGAIN: CLEAR %s", kSeqTrackNames[confirmTrack]);
    color = kColorOrange;
  } else {
    snprintf(text, sizeof(text), "PAD8 CLEAR CH  PAD4 MET  K1 BPM");
  }
  drawTextField(12, kPageHint2Y, kScreenW - 24, text, 1, color, kColorBg);
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
  // Сначала зажигаем новый столбец, потом гасим старый: так на экране нет
  // момента, когда бегущий шаг не горит нигде.
  if (next != 255) {
    drawSequencerPlayheadColumn(seq, next, true, cursorTrack, cursorStep);
  }
  if (prev != 255) {
    drawSequencerPlayheadColumn(seq, prev, false, cursorTrack, cursorStep);
  }
  if (next == 255) {
    for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
      tft_.fillCircle(8, seqTrackY(t) + kSeqCellH / 2, 3, kColorBg);
    }
  } else {
    // Гашение прошлого столбца стёрло и точки каналов — вернуть их для
    // текущего шага.
    for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
      tft_.fillCircle(8, seqTrackY(t) + kSeqCellH / 2, 3,
                      seq.isOn(t, next) ? uicolor::kTrack[t] : kColorBg);
    }
  }
}

void UiScreens::updateSequencerHeader(bool playing, uint16_t bpm, bool metronomeOn) {
  tft_.fillRect(kSeqTransportX, 10, kScreenW - 12 - kSeqTransportX, 24, kColorBg);

  const int16_t iconX = kSeqTransportX + 4;
  const int16_t iconY = 14;
  if (playing) {
    tft_.fillTriangle(iconX, iconY, iconX, iconY + 14, iconX + 12, iconY + 7, kColorGreen);
  } else {
    tft_.fillRect(iconX, iconY + 1, 12, 12, kColorHint);
  }
  drawText(iconX + 18, iconY + 4, playing ? "PLAY" : "STOP", 1,
           playing ? kColorGreen : kColorHint, kColorBg);

  // Метроном виден и здесь: PAD4 включает его с любого экрана.
  if (metronomeOn) {
    tft_.fillRoundRect(kSeqMetX, iconY, 26, 14, 3, kColorGreen);
    drawText(kSeqMetX + 4, iconY + 4, "MET", 1, kColorBg, kColorGreen);
  } else {
    tft_.drawRoundRect(kSeqMetX, iconY, 26, 14, 3, kColorDim);
    drawText(kSeqMetX + 4, iconY + 4, "MET", 1, kColorHint, kColorBg);
  }

  char bpmStr[10];
  snprintf(bpmStr, sizeof(bpmStr), "%u BPM", bpm);
  drawText(kScreenW - 12 - (int16_t)strlen(bpmStr) * 6, iconY + 4, bpmStr, 1, kColorCream,
           kColorBg);
}
