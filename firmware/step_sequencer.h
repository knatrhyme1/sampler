// Паттерн степ-секвенсора раздела 1 (драм-машина в духе Channel Rack из
// FL Studio): 4 канала (KICK/SNARE/HAT/PERC) x 16 шагов — четыре такта
// четвертями (шаг = доля, 4 шага на такт).
//
// Здесь только данные: какие удары стоят в сетке. Часы, темп и запуск
// голосов — в transport.h.
#pragma once

#include <stdint.h>

class StepSequencer {
 public:
  static const uint8_t kTracks = 4;
  static const uint8_t kSteps = 16;       // 4 такта по 4 четверти
  static const uint8_t kStepsPerBar = 4;
  static const uint8_t kStepsPerBeat = 1;  // четверти: шаг = доля

  // «Нет значения» для номера шага и канала: транспорт стоит, курсора или
  // выбора нет.
  static const uint8_t kNoStep = 255;
  static const uint8_t kNoTrack = 255;

  bool isOn(uint8_t track, uint8_t step) const {
    return (steps_[track] >> step) & 1;
  }

  void toggle(uint8_t track, uint8_t step) {
    steps_[track] ^= (uint16_t)(1u << step);
  }

  void clearTrack(uint8_t track) { steps_[track] = 0; }

  bool empty() const {
    for (uint8_t t = 0; t < kTracks; t++) {
      if (steps_[t] != 0) return false;
    }
    return true;
  }

 private:
  uint16_t steps_[kTracks] = {0, 0, 0, 0};  // бит N — шаг N включён
};
