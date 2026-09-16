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

const unsigned long BEAT_INTERVAL_MS = 1000;
const unsigned long POWER_HOLD_MS = 600;  // защита от случайного нажатия
const unsigned long BOOT_DURATION_MS = 3000;
const unsigned long BOOT_FRAME_MS = 60;  // ~16 fps перерисовки глитч-анимации
const uint16_t DEFAULT_BPM = 120;
const uint8_t DEFAULT_TRACK = 0;

enum class PowerState : uint8_t { Off, Boot, Home };

WokwiInputSource inputSource;
UiScreens ui;

unsigned long lastBeat = 0;
PowerState powerState = PowerState::Off;

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

void enterHome() {
  powerState = PowerState::Home;
  ui.showHome(DEFAULT_BPM, DEFAULT_TRACK);
  Serial.println("power: home/ready");
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
