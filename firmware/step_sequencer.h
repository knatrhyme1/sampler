// Степ-секвенсор раздела 1 (драм-машина в духе Channel Rack из FL Studio):
// 4 канала (KICK/SNARE/HAT/PERC) x 16 шагов — два такта восьмыми нотами.
// Модель паттерна и транспорт отдельно от UI (docs/poc-backlog-draft.md, D:
// "модель Pattern, отдельная от UI"). Звука пока нет — сэмплы подключаются
// отдельной стори, здесь только данные, часы и срабатывания шагов.
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

  bool playing() const { return playing_; }
  // Шаг, который сейчас звучит; имеет смысл только пока playing().
  uint8_t currentStep() const { return currentStep_; }

  // Старт всегда с первого шага (стори 1.2). Первый шаг срабатывает сразу
  // на ближайшем update(), не через интервал.
  void start(uint32_t nowUs) {
    playing_ = true;
    currentStep_ = 0;
    nextStepAtUs_ = nowUs;
    firstStepPending_ = true;
  }

  void stop() { playing_ = false; }

  // Возвращает true, если в этот вызов наступил новый шаг (currentStep()
  // уже указывает на него). Следующий момент шага считается прибавлением
  // интервала к предыдущему моменту, а не к "сейчас" — так опоздание одного
  // loop() не накапливается в уход темпа (стори 1.1). Темп читается на
  // каждом шаге, поэтому смена BPM применяется с ближайшего шага.
  bool update(uint32_t nowUs, uint16_t bpm) {
    if (!playing_) return false;
    if ((int32_t)(nowUs - nextStepAtUs_) < 0) return false;

    if (firstStepPending_) {
      firstStepPending_ = false;
    } else {
      currentStep_ = (currentStep_ + 1) % kSteps;
    }
    const uint32_t intervalUs = 60000000UL / ((uint32_t)bpm * kStepsPerBeat);
    nextStepAtUs_ += intervalUs;
    // Если loop() надолго завис (например, долгая перерисовка), не пытаемся
    // "догонять" пропущенные шаги пачкой — продолжаем от текущего момента.
    if ((int32_t)(nowUs - nextStepAtUs_) > (int32_t)intervalUs) {
      nextStepAtUs_ = nowUs + intervalUs;
    }
    return true;
  }

 private:
  uint16_t steps_[kTracks] = {0, 0, 0, 0};  // бит N — шаг N включён
  bool playing_ = false;
  bool firstStepPending_ = false;
  uint8_t currentStep_ = 0;
  uint32_t nextStepAtUs_ = 0;
};
