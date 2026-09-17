#include "audio_engine.h"

#include <math.h>

namespace {
// Запас по уровню: 4 канала на полной громкости одновременно не должны
// упираться в клиппинг мастера. ~-4 дБ на канал.
constexpr uint16_t kChannelGainQ8 = 160;
constexpr uint16_t kMetronomeGainQ8 = 256;
constexpr float kClickFreqHz = 1800.0f;
constexpr float kAccentClickFreqHz = 2400.0f;  // первая доля такта
constexpr float kClickAmplitude = 0.35f;
constexpr uint32_t kUnityStepQ16 = 1UL << 16;
}  // namespace

void AudioEngine::begin(uint32_t outputRate) {
  outputRate_ = min(outputRate, kMaxOutputRate);
  stopAll();

  // Щелчок метронома — синус с быстрым экспоненциальным спадом, считается
  // один раз под частоту вывода и дальше играет как обычный ваншот через ту
  // же мастер-шину.
  clickLength_ = (uint16_t)(outputRate_ * 30 / 1000);
  for (uint16_t i = 0; i < clickLength_; i++) {
    const float t = (float)i / outputRate_;
    const float env = exp(-t * 180.0f);
    clickNormal_[i] = (int16_t)(sin(2.0f * PI * kClickFreqHz * t) * env * kClickAmplitude * 32767);
    clickAccent_[i] =
        (int16_t)(sin(2.0f * PI * kAccentClickFreqHz * t) * env * kClickAmplitude * 32767);
  }
}

void AudioEngine::startVoice(uint8_t index, const int16_t* data, uint32_t length,
                             uint32_t stepQ16, uint16_t gainQ8) {
  portENTER_CRITICAL(&mux_);
  Voice& v = voices_[index];
  v.data = data;
  v.length = length;
  v.pos = 0;
  v.frac = 0;
  v.stepQ16 = stepQ16;
  v.gainQ8 = gainQ8;
  v.active = length > 0;
  portEXIT_CRITICAL(&mux_);
}

void AudioEngine::trigger(uint8_t channel, const OneShot& sample) {
  if (channel >= kChannelVoices) return;
  const uint32_t stepQ16 = (uint32_t)(((uint64_t)sample.sampleRate << 16) / outputRate_);
  startVoice(channel, sample.data, sample.length, stepQ16, kChannelGainQ8);
}

void AudioEngine::triggerMetronome(bool accent) {
  startVoice(kMetronomeVoice, accent ? clickAccent_ : clickNormal_, clickLength_, kUnityStepQ16,
             kMetronomeGainQ8);
}

void AudioEngine::stopAll() {
  portENTER_CRITICAL(&mux_);
  for (uint8_t i = 0; i < kVoices; i++) voices_[i].active = false;
  portEXIT_CRITICAL(&mux_);
}

uint16_t AudioEngine::takePeak() {
  portENTER_CRITICAL(&mux_);
  const uint16_t p = peak_;
  peak_ = 0;
  portEXIT_CRITICAL(&mux_);
  return p;
}

int16_t IRAM_ATTR AudioEngine::renderSample() {
  portENTER_CRITICAL_ISR(&mux_);
  int32_t mix = 0;
  for (uint8_t i = 0; i < kVoices; i++) {
    Voice& v = voices_[i];
    if (!v.active) continue;

    int32_t s = v.data[v.pos];
    if (v.frac != 0) {
      // Линейная интерполяция к следующему сэмплу. frac сдвинут до 15 бит,
      // чтобы произведение гарантированно влезало в int32.
      const int32_t next = (v.pos + 1 < v.length) ? v.data[v.pos + 1] : 0;
      s += ((next - s) * (int32_t)(v.frac >> 1)) >> 15;
    }
    mix += (s * v.gainQ8) >> 8;

    v.frac += v.stepQ16;
    v.pos += v.frac >> 16;
    v.frac &= 0xFFFF;
    if (v.pos >= v.length) v.active = false;
  }

  // Мастер-шина: общий гейн и жёсткий лимит на границах int16. Мягкий
  // лимитер — отдельная стори (эффекты мастера).
  mix = (mix * masterGainQ8_) >> 8;
  if (mix > 32767) mix = 32767;
  if (mix < -32768) mix = -32768;

  const uint16_t level = (uint16_t)(mix < 0 ? -(mix + 1) : mix);
  if (level > peak_) peak_ = level;
  portEXIT_CRITICAL_ISR(&mux_);
  return (int16_t)mix;
}
