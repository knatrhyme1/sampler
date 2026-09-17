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

void AudioEngine::postTrigger(uint8_t index, const int16_t* data, uint32_t length,
                              uint32_t stepQ16, uint16_t gainQ8) {
  portENTER_CRITICAL(&mux_);
  PendingTrigger& p = pending_[index];
  p.data = data;
  p.length = length;
  p.stepQ16 = stepQ16;
  p.gainQ8 = gainQ8;
  p.valid = true;
  portEXIT_CRITICAL(&mux_);
}

void AudioEngine::trigger(uint8_t channel, const OneShot& sample) {
  if (channel >= kChannelVoices) return;
  const uint32_t stepQ16 = (uint32_t)(((uint64_t)sample.sampleRate << 16) / outputRate_);
  postTrigger(channel, sample.data, sample.length, stepQ16, kChannelGainQ8);
}

void AudioEngine::triggerMetronome(bool accent) {
  postTrigger(kMetronomeVoice, accent ? clickAccent_ : clickNormal_, clickLength_, kUnityStepQ16,
              kMetronomeGainQ8);
}

void AudioEngine::stopAll() {
  portENTER_CRITICAL(&mux_);
  stopPending_ = true;
  for (uint8_t i = 0; i < kVoices; i++) pending_[i].valid = false;
  portEXIT_CRITICAL(&mux_);
}

uint16_t AudioEngine::takePeak() {
  portENTER_CRITICAL(&mux_);
  const uint16_t p = peak_;
  peak_ = 0;
  portEXIT_CRITICAL(&mux_);
  return p;
}

void AudioEngine::applyPending() {
  // Забираем содержимое ящика под замком и сразу отпускаем его: сам рендер
  // идёт без критической секции, иначе она обошлась бы в целый блок
  // запрещённых прерываний.
  PendingTrigger local[kVoices];
  bool stop;
  portENTER_CRITICAL(&mux_);
  stop = stopPending_;
  stopPending_ = false;
  for (uint8_t i = 0; i < kVoices; i++) {
    local[i] = pending_[i];
    pending_[i].valid = false;
  }
  portEXIT_CRITICAL(&mux_);

  if (stop) {
    for (uint8_t i = 0; i < kVoices; i++) voices_[i].active = false;
  }
  for (uint8_t i = 0; i < kVoices; i++) {
    if (!local[i].valid) continue;
    Voice& v = voices_[i];
    v.data = local[i].data;
    v.length = local[i].length;
    v.pos = 0;
    v.frac = 0;
    v.stepQ16 = local[i].stepQ16;
    v.gainQ8 = local[i].gainQ8;
    v.active = local[i].length > 0;
  }
}

void AudioEngine::renderBlock(int16_t* out, uint16_t n) {
  if (n == 0) return;
  if (n > kMaxBlockSamples) n = kMaxBlockSamples;

  applyPending();

  // Накопитель на весь блок: голос проходит блок целиком, его позиция и
  // громкость всё это время лежат в регистрах, а не перечитываются из
  // структуры на каждый сэмпл.
  int32_t acc[kMaxBlockSamples];
  for (uint16_t k = 0; k < n; k++) acc[k] = 0;

  for (uint8_t i = 0; i < kVoices; i++) {
    Voice& v = voices_[i];
    if (!v.active) continue;

    const int16_t* const data = v.data;
    const uint32_t length = v.length;
    const uint32_t stepQ16 = v.stepQ16;
    const uint16_t gainQ8 = v.gainQ8;
    uint32_t pos = v.pos;
    uint32_t frac = v.frac;

    for (uint16_t k = 0; k < n; k++) {
      int32_t s = data[pos];
      // Линейная интерполяция к следующему сэмплу. frac сдвинут до 15 бит,
      // чтобы произведение гарантированно влезало в int32.
      const int32_t next = (pos + 1 < length) ? data[pos + 1] : 0;
      s += ((next - s) * (int32_t)(frac >> 1)) >> 15;
      acc[k] += (s * gainQ8) >> 8;

      frac += stepQ16;
      pos += frac >> 16;
      frac &= 0xFFFF;
      if (pos >= length) {
        // Голос доиграл на этом сэмпле; остаток блока он уже не звучит.
        v.active = false;
        break;
      }
    }

    v.pos = pos;
    v.frac = frac;
  }

  // Мастер-шина: общий гейн и жёсткий лимит на границах int16. Мягкий
  // лимитер — отдельная стори (эффекты мастера).
  uint16_t peak = 0;
  for (uint16_t k = 0; k < n; k++) {
    int32_t mix = (acc[k] * masterGainQ8_) >> 8;
    if (mix > 32767) mix = 32767;
    if (mix < -32768) mix = -32768;
    out[k] = (int16_t)mix;

    const uint16_t level = (uint16_t)(mix < 0 ? -(mix + 1) : mix);
    if (level > peak) peak = level;
  }

  portENTER_CRITICAL(&mux_);
  if (peak > peak_) peak_ = peak;
  portEXIT_CRITICAL(&mux_);
}
