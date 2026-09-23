// Пиано-ролл (раздел 3 проекта) — заготовка интерфейса. Ноты хранятся, но
// пока не звучат: мелодического инструмента в движке ещё нет (будет сэмпл
// на клавишах с изменением высоты). Сетка та же, что у
// степ-секвенсора: 16 шагов шестнадцатыми = один такт, поэтому ролл и
// драм-каналы видны на одной временной шкале в аранжировке.
#pragma once

#include <stdint.h>

#include "mpk_mapping.h"
#include "step_sequencer.h"

class PianoRoll {
 public:
  static const uint8_t kSteps = StepSequencer::kSteps;
  // Две октавы клавиш MPK: C3..C5 (25 нот).
  static const uint8_t kLowestNote = MPK_KEY_LOWEST;
  static const uint8_t kPitches = MPK_KEY_HIGHEST - MPK_KEY_LOWEST + 1;

  bool isOn(uint8_t step, uint8_t pitch) const { return (notes_[step] >> pitch) & 1; }

  void toggle(uint8_t step, uint8_t pitch) { notes_[step] ^= (uint32_t)1 << pitch; }

  bool stepHasNotes(uint8_t step) const { return notes_[step] != 0; }

  bool empty() const {
    for (uint8_t s = 0; s < kSteps; s++) {
      if (notes_[s] != 0) return false;
    }
    return true;
  }

 private:
  uint32_t notes_[kSteps] = {};  // бит N — нота kLowestNote + N на этом шаге
};
