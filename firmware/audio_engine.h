// Звуковой движок: голоса ваншотов -> микшер -> мастер-шина.
//
// Движок не знает, куда уходит звук. Сэмплы у него забирают через
// renderBlock(): живой звук — Transport (в задаче звука), экспорт в WAV —
// PatternExporter. Частота вывода задаётся в begin().
//
// Звук считается блоками, а не по сэмплу: постоянные расходы на вызов
// (замок, разбор заявок) делятся на размер блока.
//
// renderBlock() вызывают только из задачи, не из прерывания: ваншоты лежат
// во флеше (default_samples.h), а во время записи во флеш прерыванию он
// недоступен.
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

// Настройки микшера (раздел 2): полосы 0..3 — каналы секвенсора, 4 —
// метроном, 5 — мастер. Громкость 0..100 линейно по амплитуде; 100 —
// номинальный уровень полосы, без ослабления.
struct MixerSettings {
  static const uint8_t kStrips = 6;
  static const uint8_t kMetronomeStrip = 4;
  static const uint8_t kMasterStrip = 5;
  static const uint8_t kMaxVolume = 100;

  uint8_t volume[kStrips] = {kMaxVolume, kMaxVolume, kMaxVolume,
                             kMaxVolume, kMaxVolume, kMaxVolume};
  bool muted[kStrips] = {false, false, false, false, false, false};
};

class AudioEngine {
 public:
  // Голоса 0..3 — каналы секвенсора (по одному на канал: новый удар
  // перезапускает звук канала, как choke в драм-машине), 4 — метроном.
  static const uint8_t kChannelVoices = 4;
  static const uint8_t kMetronomeVoice = 4;
  static const uint8_t kVoices = 5;
  static constexpr uint32_t kMaxOutputRate = 48000;

  // Громкость ×1,0. Громкости хранятся целыми в формате Q8: число / 256
  // (см. «Как читать числа» в audio_engine.cpp).
  static const uint16_t kUnityGainQ8 = 256;

  // Наибольший блок, который считает renderBlock() за один вызов (под
  // него рассчитан накопитель на стеке). Больший n молча обрезается до
  // этого значения, поэтому вызывающий режет длинный запрос сам — как
  // Transport::render.
  static const uint16_t kMaxBlockSamples = 64;

  // outputRate — частота, с которой будут запрашиваться сэмплы.
  // Ваншоты с другой частотой пересчитываются на лету (линейная
  // интерполяция); при совпадающей частоте сэмплы идут бит в бит.
  void begin(uint32_t outputRate);

  // Запуск и остановка голосов. Можно вызывать из любой задачи (не из
  // прерывания), в том числе изнутри рендера — так делает Transport.
  //
  // Запуск не мгновенный: заявка ложится в почтовый ящик и применяется в
  // начале ближайшего renderBlock(). Поэтому удар звучит с первого сэмпла
  // следующего блока. Transport этим пользуется: сначала trigger(), сразу
  // за ним renderBlock() с блоком, который начинается на границе шага.
  void trigger(uint8_t channel, const OneShot& sample);
  void triggerMetronome(bool accent);
  void stopAll();
  // Громкости и mute микшера. Действуют сразу, в том числе на уже
  // звучащие голоса (со следующего блока).
  void applyMixer(const MixerSettings& mix);
  // Пиковый уровень мастер-шины (0..32767) с прошлого вызова — для
  // индикатора OUT.
  uint16_t takePeak();
  // Пиковый уровень голоса после его фейдера (до мастера) с прошлого
  // вызова — для индикаторов каналов микшера.
  uint16_t takeVoicePeak(uint8_t voice);

  // Рендерит ровно n сэмплов (1..kMaxBlockSamples) в out. Вызывается из
  // задачи вывода или из экспорта — в обоих случаях это контекст задачи,
  // а не прерывания.
  void renderBlock(int16_t* out, uint16_t n);

 private:
  struct Voice {
    const int16_t* data;
    uint32_t length;
    uint32_t pos;
    uint32_t frac;     // дробная часть позиции: 0..65535 = 0..1 сэмпла (Q16)
    uint32_t stepQ16;  // на сколько сэмплов ваншота сдвинуться за выходной сэмпл (Q16)
    bool active;
  };

  // Заявка на запуск голоса, оставленная trigger() до ближайшего блока.
  // Повторный trigger() того же канала до ближайшего блока затирает
  // предыдущую заявку — как choke в драм-машине.
  struct PendingTrigger {
    const int16_t* data;
    uint32_t length;
    uint32_t stepQ16;
    bool valid;
  };

  static const uint16_t kMaxClickLength = kMaxOutputRate * 30 / 1000;  // 30 мс

  void postTrigger(uint8_t index, const int16_t* data, uint32_t length, uint32_t stepQ16);
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
  // Громкость голоса и мастера (Q8, kUnityGainQ8 = ×1,0); пишет
  // applyMixer(), читает рендер один раз на блок.
  uint16_t voiceGainQ8_[kVoices] = {};
  uint16_t masterGainQ8_ = kUnityGainQ8;
  uint16_t peak_ = 0;
  uint16_t voicePeak_[kVoices] = {};
  // Копия громкостей на время блока — принадлежит рендеру.
  uint16_t gainQ8_[kVoices] = {};
  uint16_t blockMasterQ8_ = kUnityGainQ8;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
