#include "wokwi_input_source.h"

#include <Arduino.h>

#include "mpk_mapping.h"

void WokwiInputSource::begin() {
  pinMode(pinLoad_, OUTPUT);
  pinMode(pinClock_, OUTPUT);
  pinMode(pinData_, INPUT);
  digitalWrite(pinLoad_, HIGH);
  digitalWrite(pinClock_, LOW);

  lastBits_ = readShiftRegisters();
  for (uint8_t i = 0; i < kNumPots; i++) {
    potLastSent_[i] = analogRead(potPins_[i]) >> 5;  // 12-бит ADC -> 0..127
  }
}

uint64_t WokwiInputSource::readShiftRegisters() {
  // Защёлкиваем состояние входов (PL низкий -> высокий).
  digitalWrite(pinLoad_, LOW);
  delayMicroseconds(5);
  digitalWrite(pinLoad_, HIGH);
  delayMicroseconds(5);

  // Сразу после защёлки на Q7 последней в цепи микросхемы уже стоит её D7,
  // без начального импульса CP. Дальше каждый импульс сдвигает следующий бит.
  uint64_t value = 0;
  for (uint8_t i = 0; i < kNumBits; i++) {
    value = (value << 1) | digitalRead(pinData_);
    digitalWrite(pinClock_, HIGH);
    delayMicroseconds(2);
    digitalWrite(pinClock_, LOW);
    delayMicroseconds(2);
  }
  return value;
}

bool WokwiInputSource::bitToEvent(uint8_t srNum, uint8_t dBit, bool pressed,
                                   InputEvent& ev) {
  if (srNum == 1) {
    // Пэды 1..8 (D0..D7), ноты банка A из раскладки MPK.
    ev.type = pressed ? InputEventType::NoteOn : InputEventType::NoteOff;
    ev.channel = MPK_CHANNEL_PADS;
    ev.number = MPK_PAD_NOTES_BANK_A[dBit];
    ev.value = pressed ? 127 : 0;
    return true;
  }
  if (srNum >= 2 && srNum <= 4) {
    // Клавиши 1..24 подряд от MPK_KEY_LOWEST (C3).
    const uint8_t keyIndex = (srNum - 2) * 8 + dBit;  // 0..23
    ev.type = pressed ? InputEventType::NoteOn : InputEventType::NoteOff;
    ev.channel = MPK_CHANNEL_KEYS;
    ev.number = MPK_KEY_LOWEST + keyIndex;
    ev.value = pressed ? 100 : 0;
    return true;
  }
  // srNum == 5
  if (dBit == 0) {
    // 25-я, последняя клавиша (C5 == MPK_KEY_HIGHEST).
    ev.type = pressed ? InputEventType::NoteOn : InputEventType::NoteOff;
    ev.channel = MPK_CHANNEL_KEYS;
    ev.number = MPK_KEY_HIGHEST;
    ev.value = pressed ? 100 : 0;
    return true;
  }
  if (dBit == 1 || dBit == 2) {
    // У настоящего MPK mini нет кнопок Play/Stop (см. бэклог, риск стори 3.2),
    // поэтому это не ноты, а отдельные CC для транспорта/режима.
    ev.type = InputEventType::ControlChange;
    ev.channel = MPK_CHANNEL_KEYS;
    ev.number = (dBit == 1) ? DEVICE_CC_PLAY_STOP : DEVICE_CC_MODE;
    ev.value = pressed ? 127 : 0;
    return true;
  }
  if (dBit == 3) {
    // Кнопка включения/выключения — тоже не с MPK, своя, см. docs/boot-screen-brief.md.
    ev.type = InputEventType::ControlChange;
    ev.channel = MPK_CHANNEL_KEYS;
    ev.number = DEVICE_CC_POWER;
    ev.value = pressed ? 127 : 0;
    return true;
  }
  return false;  // sr5.D4..D7 не используются
}

bool WokwiInputSource::poll(InputEvent& ev) {
  const uint64_t bits = readShiftRegisters();
  const uint64_t changed = bits ^ lastBits_;
  if (changed != 0) {
    for (uint8_t i = 0; i < kNumBits; i++) {
      const uint64_t mask = (uint64_t)1 << i;
      if (!(changed & mask)) continue;

      // Кнопка тянет вход в LOW (пул-ап резистор к VCC на неотжатом входе).
      const bool pressed = (bits & mask) == 0;
      lastBits_ = (lastBits_ & ~mask) | (bits & mask);

      // i — позиция бита в 40-битном слове (0 — младший, т.е. прочитанный
      // последним). Переводим её в (номер микросхемы, номер входа D0..D7),
      // зная порядок цепи: esp -> sr5 -> sr4 -> sr3 -> sr2 -> sr1.
      const uint8_t k = kNumBits - 1 - i;         // 0 — бит, прочитанный первым
      const uint8_t chipOffset = k / 8;           // 0 для sr5, ..., 4 для sr1
      const uint8_t srNum = kNumShiftRegisters - chipOffset;
      const uint8_t dBit = 7 - (k % 8);

      if (bitToEvent(srNum, dBit, pressed, ev)) {
        return true;
      }
    }
  }

  for (uint8_t i = 0; i < kNumPots; i++) {
    const int value = analogRead(potPins_[i]) >> 5;  // 0..127
    if (abs(value - potLastSent_[i]) >= 2) {
      potLastSent_[i] = value;
      ev.type = InputEventType::ControlChange;
      ev.channel = MPK_CHANNEL_KNOBS;
      ev.number = MPK_KNOB_CC_BASE + i;
      ev.value = (uint8_t)value;
      return true;
    }
  }
  return false;
}
