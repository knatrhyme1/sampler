// Звуковой движок: голоса ваншотов -> микшер -> мастер-шина (стори 4.x,
// путь StepEvent -> Engine -> Voices -> Mix -> Output из
// docs/poc-backlog-draft.md, E.1). Не знает, куда уходит звук: сэмплы
// забирает вывод (живой звук — audio_output_1bit.h в Wokwi, на железе —
// I2S-кодек; экспорт — pattern_export.h), вызывая renderSample() с частотой,
// заданной в begin().
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

  // outputRate — частота, с которой будет вызываться renderSample().
  // Ваншоты с другой частотой пересчитываются на лету (линейная
  // интерполяция); при совпадающей частоте сэмплы идут бит в бит.
  void begin(uint32_t outputRate);

  // Вызываются из loop().
  void trigger(uint8_t channel, const OneShot& sample);
  void triggerMetronome(bool accent);
  void stopAll();
  // Пиковый уровень мастер-шины (0..32767) с прошлого вызова — для
  // индикатора OUT.
  uint16_t takePeak();

  // Вызывается из прерывания вывода (или из экспорта), один раз на сэмпл.
  int16_t renderSample();  // в IRAM — см. определение

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

  static const uint16_t kMaxClickLength = kMaxOutputRate * 30 / 1000;  // 30 мс

  void startVoice(uint8_t index, const int16_t* data, uint32_t length, uint32_t stepQ16,
                  uint16_t gainQ8);

  uint32_t outputRate_ = kAudioSampleRate;
  Voice voices_[kVoices] = {};
  uint16_t clickLength_ = 0;
  int16_t clickNormal_[kMaxClickLength] = {};
  int16_t clickAccent_[kMaxClickLength] = {};
  uint16_t masterGainQ8_ = 256;
  uint16_t peak_ = 0;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
