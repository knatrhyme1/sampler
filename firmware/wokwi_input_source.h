// Читает виртуальный контроллер в Wokwi: пэды и клавиши — через цепочку
// из пяти 74HC165 (сдвиговые регистры), крутилки — напрямую через ADC.
// Раскладка пинов и порядок бит соответствуют firmware/diagram.json.
#pragma once

#include <stdint.h>

#include "input_source.h"

class WokwiInputSource : public InputSource {
 public:
  void begin() override;
  bool poll(InputEvent& ev) override;

 private:
  static const uint8_t kNumShiftRegisters = 5;
  static const uint8_t kNumBits = kNumShiftRegisters * 8;
  static const uint8_t kNumPots = 8;

  const uint8_t pinLoad_ = 15;
  const uint8_t pinClock_ = 16;
  const uint8_t pinData_ = 17;
  const uint8_t potPins_[kNumPots] = {1, 2, 4, 5, 6, 7, 8, 9};

  uint64_t lastBits_ = 0;
  int potLastSent_[kNumPots] = {-1, -1, -1, -1, -1, -1, -1, -1};

  uint64_t readShiftRegisters();
  // srNum: 1..5 (номер микросхемы в цепи, sr1 — самая дальняя от платы).
  // dBit: 0..7 (номер входа D0..D7 на этой микросхеме).
  bool bitToEvent(uint8_t srNum, uint8_t dBit, bool pressed, InputEvent& ev);
};
