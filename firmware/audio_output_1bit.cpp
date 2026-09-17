#include "audio_output_1bit.h"

#include <soc/gpio_struct.h>

namespace {
// Один бит на сэмпл движка. Таймер: 80 МГц / 98 = 816 326 Гц, аларм каждые
// 51 тик = 16 006 Гц (на 0,04% быстрее kAudioSampleRate — на слух не
// отличить; в Wokwi лог heartbeat показывает ~15 900 сэмплов в секунду).
constexpr uint32_t kTimerResolutionHz = 80000000UL / 98;
constexpr uint32_t kTimerTicksPerSample = 51;
// Порог гистерезиса (~-42 дБ): тихие хвосты не дребезжат пином туда-сюда.
constexpr int16_t kThreshold = 256;
// Сколько подряд "тихих" сэмплов до того, как пин уходит в ноль (~4 мс).
constexpr uint16_t kSilenceSamples = kAudioSampleRate / 250;
// Если пин долго не меняется, пищалка Wokwi перестаёт присылать звук, а
// после паузы её рендер отстаёт: каждый новый перепад "догоняет" тишину
// кусками по 100 мс, и удар в 300 мс растягивается в бесконечную
// пульсацию (проверено записью: тон 40–200 Гц без пауз рендерится точно,
// удар после тишины — нет). Поэтому раз в ~1 мс пин дёргается туда и
// обратно внутри одного прерывания: для пищалки это событие, держащее её
// время актуальным, а в отсчёты 48 кГц импульс короче микросекунды не
// попадает.
constexpr uint16_t kKeepAliveSamples = kAudioSampleRate / 1000;

AudioEngine* gEngine = nullptr;
uint32_t gPinMask = 0;
hw_timer_t* gTimer = nullptr;
bool gHigh = false;
uint16_t gQuietCount = 0;
uint16_t gKeepAliveCount = 0;
volatile uint32_t gSamplesOut = 0;
volatile uint32_t gEdges = 0;
}  // namespace

uint32_t OneBitAudioOutput::samplesOut() const { return gSamplesOut; }
uint32_t OneBitAudioOutput::edges() const { return gEdges; }

void OneBitAudioOutput::begin(AudioEngine& engine, uint8_t pin) {
  gEngine = &engine;
  gPinMask = 1UL << pin;  // GPIO0..31 — регистр out_w1ts/out_w1tc
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);

  gTimer = timerBegin(kTimerResolutionHz);
  timerAttachInterrupt(gTimer, &OneBitAudioOutput::onTimer);
  timerAlarm(gTimer, kTimerTicksPerSample, true, 0);
}

void IRAM_ATTR OneBitAudioOutput::onTimer() {
  const int16_t s = gEngine->renderSample();
  gSamplesOut = gSamplesOut + 1;

  if (s > kThreshold) {
    gHigh = true;
    gQuietCount = 0;
  } else if (s < -kThreshold) {
    gHigh = false;
    gQuietCount = 0;
  } else if (gQuietCount < kSilenceSamples) {
    gQuietCount++;
  } else {
    gHigh = false;  // тишина: пин в нуле, пищалка молчит
  }

  static bool wasHigh = false;
  if (gHigh != wasHigh) {
    gEdges = gEdges + 1;
    wasHigh = gHigh;
  }

  if (++gKeepAliveCount >= kKeepAliveSamples) {
    gKeepAliveCount = 0;
    // Импульс в противоположное состояние; ниже пин сразу вернётся обратно.
    if (gHigh) {
      GPIO.out_w1tc = gPinMask;
    } else {
      GPIO.out_w1ts = gPinMask;
    }
  }
  if (gHigh) {
    GPIO.out_w1ts = gPinMask;
  } else {
    GPIO.out_w1tc = gPinMask;
  }
}
