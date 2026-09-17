// Транспорт проекта: часы, темп, метроном и запуск голосов по паттерну.
//
// Музыкальное время считается в сэмплах звукового вывода и живёт в задаче
// звука на ядре 0, а не в loop() по millis(). Раньше шаг наступал в
// loop(), поэтому полная перерисовка экрана (40–50 мс) сдвигала долю
// такта, а защита от опоздания в часах переякоривала сетку и паттерн терял
// фазу (docs/known-issues.md, п. 4). Теперь отрисовка на ядре 1 сдвинуть
// долю не может физически, а блок рендера обрывается ровно на границе
// шага, поэтому удар попадает на свой сэмпл — так же, как в офлайн-рендере
// экспорта.
//
// Разделение ролей:
//   UI (ядро 1)          -> play/stop, темп, метроном, правка паттерна
//   задача звука (ядро 0) -> render(): часы, запуск голосов, бегущий шаг
//
// Всё, что UI пишет, снимается под коротким замком один раз на блок;
// сам рендер идёт без замка.
#pragma once

#include <Arduino.h>

#include "audio_engine.h"
#include "audio_source.h"
#include "step_sequencer.h"

class Transport : public AudioSource {
 public:
  // Очередь сработавших шагов для лога и экрана. Если loop() занят
  // отрисовкой и не успевает её разбирать, события теряются — на звук это
  // не влияет, теряется только строка в логе.
  static const uint8_t kFiredQueue = 8;
  static const uint8_t kPlayheadIdle = 255;

  void begin(uint32_t sampleRate, AudioEngine& engine, const OneShot* kit);

  // --- вызывается из UI (ядро 1) ---
  void play();   // всегда с первого шага (стори 1.2)
  void stop();   // уже звучащие удары доигрывают свой хвост
  bool playing() const;
  void setBpm(uint16_t bpm);  // применяется со следующего шага
  void setMetronome(bool on);
  void toggleStep(uint8_t track, uint8_t step);
  // Снимок паттерна для отрисовки и экспорта — читается, не мешая звуку.
  StepSequencer pattern() const;
  // Шаг, который сейчас звучит, либо kPlayheadIdle, если транспорт стоит.
  uint8_t playhead() const;
  // Разобрать очередь сработавших шагов. mask — биты сработавших каналов.
  bool takeFired(uint8_t& step, uint8_t& mask);

  // --- вызывается из задачи звука (ядро 0) ---
  void render(int16_t* out, uint16_t n) override;

 private:
  void setTempo(uint16_t bpm);
  // Длина очередного шага в сэмплах. Целочисленное деление здесь дало бы
  // дрейф темпа: на 137 BPM шаг ровно 3503,65 сэмпла, и округление вниз
  // уводит сетку на 10 мс меньше чем за минуту. Поэтому остаток от деления
  // копится и раз в несколько шагов добавляет лишний сэмпл — граница шага
  //k встаёт ровно в floor(k * rate * 60 / (bpm * kStepsPerBeat)), то есть
  // туда же, куда её ставит офлайн-рендер экспорта.
  uint32_t nextStepSamples();
  void fireStep(const StepSequencer& pattern, bool metronome);
  void pushFired(uint8_t step, uint8_t mask);

  AudioEngine* engine_ = nullptr;
  const OneShot* kit_ = nullptr;
  uint32_t sampleRate_ = kAudioSampleRate;

  // Пишет UI, читает задача звука — под mux_.
  StepSequencer pattern_;
  uint16_t bpm_ = 120;
  bool metronome_ = false;
  bool playing_ = false;
  bool restart_ = false;

  // Принадлежит задаче звука, замок не нужен.
  uint8_t step_ = 0;
  uint32_t stepSamples_ = 0;
  uint32_t intoStep_ = 0;
  bool pendingFire_ = false;
  uint16_t tempoBpm_ = 0;   // темп, под который посчитано деление ниже
  uint32_t stepBase_ = 0;   // целая часть длины шага
  uint32_t stepRem_ = 0;    // остаток от деления
  uint32_t stepDiv_ = 1;    // делитель (bpm * kStepsPerBeat)
  uint32_t remAcc_ = 0;     // накопленный остаток

  // Пишет задача звука, читает UI.
  volatile uint8_t playhead_ = kPlayheadIdle;
  uint8_t firedStep_[kFiredQueue] = {};
  uint8_t firedMask_[kFiredQueue] = {};
  volatile uint8_t firedHead_ = 0;  // двигает задача звука
  volatile uint8_t firedTail_ = 0;  // двигает UI

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
