// Паттерн степ-секвенсора раздела 1 (драм-машина в духе Channel Rack из
// FL Studio): 4 канала (KICK/SNARE/HAT/PERC) x 16 шагов — два такта
// восьмыми нотами. Модель Pattern, отдельная от UI
// (docs/archive/poc-backlog-draft.md, D).
//
// Здесь только данные: какие удары стоят в сетке. Часы, темп и запуск
// голосов — в transport.h: музыкальное время считается в сэмплах звукового
// вывода, а не по millis() в loop(), иначе перерисовка экрана сдвигает
// долю такта (docs/known-issues.md, п. 4).
#pragma once

#include <stdint.h>

class StepSequencer {
 public:
  static const uint8_t kTracks = 4;
  static const uint8_t kSteps = 16;       // 2 такта по 8 восьмых
  static const uint8_t kStepsPerBar = 8;
  static const uint8_t kStepsPerBeat = 2;  // восьмые: 2 шага на четверть

  bool isOn(uint8_t track, uint8_t step) const {
    return (steps_[track] >> step) & 1;
  }

  void toggle(uint8_t track, uint8_t step) {
    steps_[track] ^= (uint16_t)(1u << step);
  }

  // Биты всех шагов канала разом — транспорту удобнее снимать их одним
  // словом, чем опрашивать по одному под замком.
  uint16_t trackBits(uint8_t track) const { return steps_[track]; }

  void clear() {
    for (uint8_t t = 0; t < kTracks; t++) steps_[t] = 0;
  }

 private:
  uint16_t steps_[kTracks] = {0, 0, 0, 0};  // бит N — шаг N включён
};
