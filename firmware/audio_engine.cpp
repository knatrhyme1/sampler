#include "audio_engine.h"

#include <math.h>

// Как читать числа в этом файле.
//
// Дробные величины здесь хранятся целыми числами, умноженными на степень
// двойки (фиксированная точка) — целые процессор считает быстрее:
//
//   Q8  — число / 256.   Громкость: 256 = ×1,0, 128 = ×0,5.
//   Q16 — число / 65536. Позиция и шаг внутри ваншота: 65536 = 1 сэмпл.
//
// Сдвиг вправо на N бит — деление на 2^N с округлением вниз, поэтому
// (s * gainQ8) >> 8 читается как «s, умноженное на громкость».
//
// Сэмплы — int16 (-32768..32767), промежуточные суммы — int32. Пределы
// подобраны так, чтобы int32 не переполнялся, — см. комментарии у формул.

namespace {
// Запас по уровню: 4 канала на полной громкости одновременно не должны
// упираться в клиппинг мастера. ~-4 дБ на канал.
constexpr uint16_t kChannelGainQ8 = 160;
constexpr uint16_t kMetronomeGainQ8 = AudioEngine::kUnityGainQ8;
constexpr float kClickFreqHz = 1800.0f;
constexpr float kAccentClickFreqHz = 2400.0f;  // первая доля такта
constexpr float kClickAmplitude = 0.35f;
constexpr uint32_t kUnityStepQ16 = 65536;  // шаг 1 сэмпл за сэмпл: частоты совпадают
}  // namespace

void AudioEngine::begin(uint32_t outputRate) {
  outputRate_ = min(outputRate, kMaxOutputRate);
  stopAll();
  applyMixer(MixerSettings());

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
                              uint32_t stepQ16) {
  portENTER_CRITICAL(&mux_);
  PendingTrigger& p = pending_[index];
  p.data = data;
  p.length = length;
  p.stepQ16 = stepQ16;
  p.valid = true;
  portEXIT_CRITICAL(&mux_);
}

void AudioEngine::trigger(uint8_t channel, const OneShot& sample) {
  if (channel >= kChannelVoices) return;
  // Ваншот 44,1 кГц на выводе 16 кГц: за выходной сэмпл проходим 2,76
  // сэмпла ваншота.
  const uint32_t stepQ16 = (uint32_t)((uint64_t)sample.sampleRate * kUnityStepQ16 / outputRate_);
  postTrigger(channel, sample.data, sample.length, stepQ16);
}

void AudioEngine::triggerMetronome(bool accent) {
  postTrigger(kMetronomeVoice, accent ? clickAccent_ : clickNormal_, clickLength_, kUnityStepQ16);
}

void AudioEngine::applyMixer(const MixerSettings& mix) {
  auto scaled = [&](uint8_t strip, uint16_t unity) -> uint16_t {
    if (mix.muted[strip]) return 0;
    const uint8_t vol = min(mix.volume[strip], MixerSettings::kMaxVolume);
    return (uint16_t)((uint32_t)unity * vol / MixerSettings::kMaxVolume);
  };
  uint16_t gains[kVoices];
  for (uint8_t i = 0; i < kChannelVoices; i++) gains[i] = scaled(i, kChannelGainQ8);
  gains[kMetronomeVoice] = scaled(MixerSettings::kMetronomeStrip, kMetronomeGainQ8);
  const uint16_t master = scaled(MixerSettings::kMasterStrip, kUnityGainQ8);

  portENTER_CRITICAL(&mux_);
  for (uint8_t i = 0; i < kVoices; i++) voiceGainQ8_[i] = gains[i];
  masterGainQ8_ = master;
  portEXIT_CRITICAL(&mux_);
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

uint16_t AudioEngine::takeVoicePeak(uint8_t voice) {
  if (voice >= kVoices) return 0;
  portENTER_CRITICAL(&mux_);
  const uint16_t p = voicePeak_[voice];
  voicePeak_[voice] = 0;
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
  for (uint8_t i = 0; i < kVoices; i++) gainQ8_[i] = voiceGainQ8_[i];
  blockMasterQ8_ = masterGainQ8_;
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
  uint16_t voicePeaks[kVoices] = {};

  for (uint8_t i = 0; i < kVoices; i++) {
    Voice& v = voices_[i];
    if (!v.active) continue;

    const int16_t* const data = v.data;
    const uint32_t length = v.length;
    const uint32_t stepQ16 = v.stepQ16;
    const uint16_t gainQ8 = gainQ8_[i];
    uint32_t pos = v.pos;
    uint32_t frac = v.frac;
    int32_t vpeak = 0;

    for (uint16_t k = 0; k < n; k++) {
      // Точка чтения лежит между сэмплами pos и pos + 1, на доле frac
      // пути. Берём оба и смешиваем пропорционально (линейная
      // интерполяция): s + (next - s) * доля.
      int32_t s = data[pos];
      const int32_t next = (pos + 1 < length) ? data[pos + 1] : 0;
      // Долю берём с точностью 15 бит, а не 16: |next - s| <= 65535, и
      // 65535 * 32767 < 2^31 — произведение влезает в int32.
      s += ((next - s) * (int32_t)(frac >> 1)) >> 15;
      const int32_t g = (s * gainQ8) >> 8;  // громкость голоса (Q8)
      acc[k] += g;
      const int32_t mag = g < 0 ? -g : g;
      if (mag > vpeak) vpeak = mag;

      // Шаг вперёд: целые сэмплы из дроби переходят в pos, в frac
      // остаётся дробная часть (младшие 16 бит).
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
    voicePeaks[i] = (uint16_t)min(vpeak, (int32_t)32767);
  }

  // Мастер-шина: общая громкость (Q8) и жёсткое ограничение по границам
  // int16. Мягкого лимитера пока нет.
  uint16_t peak = 0;
  for (uint16_t k = 0; k < n; k++) {
    int32_t mix = (acc[k] * blockMasterQ8_) >> 8;
    if (mix > 32767) mix = 32767;
    if (mix < -32768) mix = -32768;
    out[k] = (int16_t)mix;

    const uint16_t level = (uint16_t)(mix < 0 ? -(mix + 1) : mix);
    if (level > peak) peak = level;
  }

  portENTER_CRITICAL(&mux_);
  if (peak > peak_) peak_ = peak;
  for (uint8_t i = 0; i < kVoices; i++) {
    if (voicePeaks[i] > voicePeak_[i]) voicePeak_[i] = voicePeaks[i];
  }
  portEXIT_CRITICAL(&mux_);
}
