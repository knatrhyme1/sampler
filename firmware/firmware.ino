// Стори 0.2: пустой проект на ESP32-S3, живой в Wokwi.
// Раз в секунду выводит сообщение в лог — доказательство, что плата
// запускается и код работает.
//
// Плюс черновая проверка виртуального контроллера в Wokwi (стори 3.1/3.2):
// WokwiInputSource реализует общий интерфейс InputSource, поэтому его можно
// будет заменить на чтение настоящего MPK mini по USB-MIDI (стори 3.3), не
// трогая остальной код.
//
// Плюс экран включения/загрузки/приветствия (см. docs/boot-screen-brief.md):
// кнопка POWER (CC DEVICE_CC_POWER) переключает устройство между "выключено"
// и загрузкой, после которой показывается главный экран.

#include "wokwi_input_source.h"
#include "ui_screens.h"
#include "control_layout.h"

const unsigned long BEAT_INTERVAL_MS = 1000;
const unsigned long POWER_HOLD_MS = 600;  // защита от случайного нажатия
const unsigned long BOOT_DURATION_MS = 3000;
const unsigned long BOOT_FRAME_MS = 60;  // ~16 fps перерисовки глитч-анимации
const uint16_t DEFAULT_BPM = 120;
const uint8_t DEFAULT_TRACK = 0;

enum class PowerState : uint8_t { Off, Boot, Home };

// Экраны внутри PowerState::Home (B.2): главный экран или меню (список
// пунктов / открытый пункт). SHIFT (удержание MODE) пока не реализован —
// короткое нажатие MODE всегда переключает Home <-> меню.
enum class UiMode : uint8_t { HomeMain, MenuList, MenuItem };

WokwiInputSource inputSource;
UiScreens ui;

unsigned long lastBeat = 0;
PowerState powerState = PowerState::Off;
UiMode uiMode = UiMode::HomeMain;
uint8_t currentTrack = DEFAULT_TRACK;
// Селектор главного экрана один: либо на BPM (общий для проекта), либо на
// ряду дорожек.
bool homeBpmFocused = false;
uint8_t menuSelected = 0;
uint8_t menuOpenItem = 0;

bool powerButtonDown = false;
unsigned long powerButtonDownAt = 0;
bool powerActionTriggered = false;

unsigned long bootStartedAt = 0;
unsigned long bootFrameShownAt = 0;

void enterOff() {
  powerState = PowerState::Off;
  ui.showOff();
  Serial.println("power: off");
}

void enterBoot() {
  powerState = PowerState::Boot;
  bootStartedAt = millis();
  bootFrameShownAt = 0;
  Serial.println("power: boot");
}

void renderUiMode() {
  switch (uiMode) {
    case UiMode::HomeMain:
      ui.showHome(DEFAULT_BPM, currentTrack, homeBpmFocused);
      break;
    case UiMode::MenuList:
      ui.showMenuList(menuSelected);
      break;
    case UiMode::MenuItem:
      ui.showMenuItem(menuOpenItem);
      break;
  }
}

void enterHome() {
  powerState = PowerState::Home;
  uiMode = UiMode::HomeMain;
  renderUiMode();
  Serial.println("power: home/ready");
}

// MODE (короткое нажатие) переключает Home <-> список меню. Удержание
// (SHIFT) — открытый вопрос B.4, пока не реализовано.
void handleModeButton(const InputEvent& ev) {
  if (ev.type != InputEventType::ControlChange || ev.number != DEVICE_CC_MODE) return;
  if (ev.value == 0) return;  // реагируем на нажатие, не на отпускание
  if (powerState != PowerState::Home) return;

  if (uiMode == UiMode::HomeMain) {
    uiMode = UiMode::MenuList;
    menuSelected = 0;
  } else {
    uiMode = UiMode::HomeMain;
  }
  renderUiMode();
}

// Навигационный кластер (B.2/B.4, docs/poc-backlog-draft.md): один и тот же
// набор команд ведёт себя по-разному в зависимости от текущего экрана.
void handleNavCommand(NavCommand cmd) {
  if (cmd == NavCommand::None || powerState != PowerState::Home) return;

  switch (uiMode) {
    case UiMode::HomeMain:
      // PAD6/PAD2 переносят селектор между BPM и рядом дорожек. PAD1/PAD3 —
      // горизонтальный смысл (влево/вправо, B.4): листают дорожку, пока
      // селектор на ряду дорожек.
      if (cmd == NavCommand::Up && !homeBpmFocused) {
        homeBpmFocused = true;
        renderUiMode();
      } else if (cmd == NavCommand::Down && homeBpmFocused) {
        homeBpmFocused = false;
        renderUiMode();
      } else if (homeBpmFocused) {
        break;
      } else if (cmd == NavCommand::Back) {
        currentTrack = (currentTrack == 0) ? 3 : currentTrack - 1;
        renderUiMode();
      } else if (cmd == NavCommand::Forward) {
        currentTrack = (currentTrack + 1) % 4;
        renderUiMode();
      }
      break;

    case UiMode::MenuList:
      if (cmd == NavCommand::Up) {
        menuSelected = (menuSelected == 0) ? 3 : menuSelected - 1;
        renderUiMode();
      } else if (cmd == NavCommand::Down) {
        menuSelected = (menuSelected + 1) % 4;
        renderUiMode();
      } else if (cmd == NavCommand::Forward || cmd == NavCommand::Confirm) {
        uiMode = UiMode::MenuItem;
        menuOpenItem = menuSelected;
        renderUiMode();
      } else if (cmd == NavCommand::Back || cmd == NavCommand::Cancel) {
        uiMode = UiMode::HomeMain;
        renderUiMode();
      }
      break;

    case UiMode::MenuItem:
      if (cmd == NavCommand::Back || cmd == NavCommand::Cancel) {
        uiMode = UiMode::MenuList;
        renderUiMode();
      }
      break;
  }
}

void updateBoot() {
  const unsigned long elapsed = millis() - bootStartedAt;
  if (elapsed >= BOOT_DURATION_MS) {
    enterHome();
    return;
  }
  if (millis() - bootFrameShownAt >= BOOT_FRAME_MS) {
    bootFrameShownAt = millis();
    const uint8_t percent = (uint8_t)((elapsed * 100) / BOOT_DURATION_MS);
    ui.showBoot(percent);
  }
}

void handlePowerButton(const InputEvent& ev) {
  if (ev.type != InputEventType::ControlChange || ev.number != DEVICE_CC_POWER) return;
  if (ev.value > 0) {
    powerButtonDown = true;
    powerButtonDownAt = millis();
    powerActionTriggered = false;
  } else {
    powerButtonDown = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);  // дать монитору порта время открыться в Wokwi
  Serial.println("ESP32-S3 sampler-sequencer: старт");
  inputSource.begin();
  ui.begin();
  enterOff();
}

void loop() {
  unsigned long now = millis();
  if (now - lastBeat >= BEAT_INTERVAL_MS) {
    lastBeat = now;
    Serial.println("heartbeat");
  }

  InputEvent ev;
  if (inputSource.poll(ev)) {
    handlePowerButton(ev);
    handleModeButton(ev);
    if (ev.type == InputEventType::NoteOn) {
      handleNavCommand(navCommandForNoteOn(ev.channel, ev.number));
    }
    switch (ev.type) {
      case InputEventType::NoteOn:
        Serial.printf("NoteOn  ch=%d note=%d vel=%d\n", ev.channel, ev.number, ev.value);
        break;
      case InputEventType::NoteOff:
        Serial.printf("NoteOff ch=%d note=%d\n", ev.channel, ev.number);
        break;
      case InputEventType::ControlChange:
        Serial.printf("CC      ch=%d cc=%d val=%d\n", ev.channel, ev.number, ev.value);
        break;
    }
  }

  // Долгое удержание POWER (>= POWER_HOLD_MS) переключает питание. Во время
  // загрузки кнопка игнорируется — процесс нельзя прервать.
  if (powerButtonDown && !powerActionTriggered && (now - powerButtonDownAt) >= POWER_HOLD_MS) {
    powerActionTriggered = true;
    if (powerState == PowerState::Off) {
      enterBoot();
    } else if (powerState == PowerState::Home) {
      enterOff();
    }
  }

  if (powerState == PowerState::Boot) {
    updateBoot();
  }
}
