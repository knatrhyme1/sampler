// Звуковой движок: голоса ваншотов -> микшер -> мастер-шина (стори 4.x,
// путь StepEvent -> Engine -> Voices -> Mix -> Output из
// docs/archive/poc-backlog-draft.md, E.1). Не знает, куда уходит звук: сэмплы
// забирает вывод (живой звук — audio_output_1bit.h в Wokwi, на железе —
// I2S-кодек; экспорт — pattern_export.h), вызывая renderBlock() с частотой,
// заданной в begin().
//
// Рендер блочный, а не посэмпловый. Раньше вывод дёргал движок из
// прерывания таймера на каждый сэмпл: на полезную работу приходилось ~16%
// затрат, остальное — вход в прерывание, диспетчер gptimer и критическая
// секция, которая стоила столько же, сколько сам микс
// (docs/known-issues.md, п. 5). Теперь потребитель просит сразу
// kMaxBlockSamples сэмплов, и постоянные накладные расходы делятся на
// размер блока.
//
// Важное следствие: renderBlock() вызывается из задачи, а не из
// прерывания, поэтому ему можно читать сэмплы из флеша (default_samples.h
// лежит в PROGMEM). Из прерывания это приводило к отказу при любой записи
// во флеш (docs/known-issues.md, п. 6).
#pragma once

#include <Arduino.h>

// Частота живого вывода в Wokwi: симулятор на каждый сэмпл выполняет
// прерывание таймера, и на 44,1 кГц симуляция заметно проседает. Экспорт в
// WAV рендерит тот же движок на 44,1 кГц.
constexpr uint32_t kAudioSampleRate = 16000;

struct OneShot {
  const int16_t* data;
  uint32_t length;
  uint32_t sampleRate;  // частота, в которой записан ваншот
};

class AudioEngine {
 public:
  // Голоса 0..3 — каналы секвенсора (по одному на канал: новый удар
  // перезапускает звук канала, как choke в драм-машине), 4 — метроном.
  static const uint8_t kChannelVoices = 4;
  static const uint8_t kMetronomeVoice = 4;
  static const uint8_t kVoices = 5;
  static constexpr uint32_t kMaxOutputRate = 48000;

  // Наибольший блок, который принимает renderBlock(). Ограничение нужно
  // только под размер накопителя на стеке; потребитель волен просить
  // меньше.
  static const uint16_t kMaxBlockSamples = 64;

  // outputRate — частота, с которой будут запрашиваться сэмплы.
  // Ваншоты с другой частотой пересчитываются на лету (линейная
  // интерполяция); при совпадающей частоте сэмплы идут бит в бит.
  void begin(uint32_t outputRate);

  // Вызываются из управляющего кода (loop() или задача UI), не из рендера.
  // Запуск голоса не мгновенный: заявка кладётся в почтовый ящик и
  // разбирается в начале ближайшего блока, то есть квантуется по границе
  // блока (при 32 сэмплах — 2 мс на 16 кГц, 0,67 мс на 48 кГц).
  void trigger(uint8_t channel, const OneShot& sample);
  void triggerMetronome(bool accent);
  void stopAll();
  // Пиковый уровень мастер-шины (0..32767) с прошлого вызова — для
  // индикатора OUT.
  uint16_t takePeak();

  // Рендерит ровно n сэмплов (1..kMaxBlockSamples) в out. Вызывается из
  // задачи вывода или из экспорта — в обоих случаях это контекст задачи,
  // а не прерывания.
  void renderBlock(int16_t* out, uint16_t n);

 private:
  struct Voice {
    const int16_t* data;
    uint32_t length;
    uint32_t pos;
    uint32_t frac;     // дробная часть позиции, 16 бит
    uint32_t stepQ16;  // шаг по ваншоту на один выходной сэмпл, 1<<16 = 1:1
    uint16_t gainQ8;   // 256 = 0 дБ
    bool active;
  };

  // Заявка на запуск голоса, оставленная trigger() до ближайшего блока.
  // Повторный trigger() того же канала внутри одного блока затирает
  // предыдущую заявку — это то же поведение choke, что и раньше.
  struct PendingTrigger {
    const int16_t* data;
    uint32_t length;
    uint32_t stepQ16;
    uint16_t gainQ8;
    bool valid;
  };

  static const uint16_t kMaxClickLength = kMaxOutputRate * 30 / 1000;  // 30 мс

  void postTrigger(uint8_t index, const int16_t* data, uint32_t length, uint32_t stepQ16,
                   uint16_t gainQ8);
  // Разбирает почтовый ящик в начале блока; выполняется в контексте
  // рендера.
  void applyPending();

  uint32_t outputRate_ = kAudioSampleRate;
  Voice voices_[kVoices] = {};
  PendingTrigger pending_[kVoices] = {};
  bool stopPending_ = false;
  uint16_t clickLength_ = 0;
  int16_t clickNormal_[kMaxClickLength] = {};
  int16_t clickAccent_[kMaxClickLength] = {};
  uint16_t masterGainQ8_ = 256;
  uint16_t peak_ = 0;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
