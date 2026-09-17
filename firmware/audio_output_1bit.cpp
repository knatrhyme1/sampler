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

// Задача звука живёт на ядре 0: ядро 1 занято loop() с отрисовкой экрана,
// а полная перерисовка блокирует его на десятки миллисекунд
// (docs/known-issues.md, п. 4). На отдельном ядре звук этого не замечает.
constexpr BaseType_t kAudioTaskCore = 0;
constexpr uint32_t kAudioTaskStack = 4096;
constexpr UBaseType_t kAudioTaskPriority = 10;
// Страховка на случай потерянного уведомления: задача всё равно проснётся
// и досыплет буфер.
constexpr TickType_t kFillTimeoutTicks = pdMS_TO_TICKS(20);

constexpr uint16_t kRingMask = OneBitAudioOutput::kRingSamples - 1;
static_assert((OneBitAudioOutput::kRingSamples & kRingMask) == 0,
              "kRingSamples должен быть степенью двойки — индекс считается маской");
static_assert(OneBitAudioOutput::kBlockSamples <= AudioEngine::kMaxBlockSamples,
              "блок вывода не должен превышать максимальный блок движка");

AudioEngine* gEngine = nullptr;
uint32_t gPinMask = 0;
hw_timer_t* gTimer = nullptr;
TaskHandle_t gTask = nullptr;

// Кольцевой буфер в DRAM. Пишет только задача (gHead), читает только
// прерывание (gTail); счётчики сквозные, индекс берётся маской.
int16_t gRing[OneBitAudioOutput::kRingSamples] = {};
volatile uint32_t gHead = 0;
volatile uint32_t gTail = 0;

bool gHigh = false;
bool gWasHigh = false;
uint16_t gQuietCount = 0;
uint16_t gKeepAliveCount = 0;
uint16_t gRefillCount = 0;
volatile uint32_t gSamplesOut = 0;
volatile uint32_t gEdges = 0;
volatile uint32_t gUnderruns = 0;
}  // namespace

uint32_t OneBitAudioOutput::samplesOut() const { return gSamplesOut; }
uint32_t OneBitAudioOutput::edges() const { return gEdges; }
uint32_t OneBitAudioOutput::underruns() const { return gUnderruns; }

void OneBitAudioOutput::begin(AudioEngine& engine, uint8_t pin) {
  gEngine = &engine;
  gPinMask = 1UL << pin;  // GPIO0..31 — регистр out_w1ts/out_w1tc
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);

  // Таймер поднимает сама задача — так его прерывание регистрируется на
  // ядре 0, рядом с рендером, и не мешает SPI-обменам экрана на ядре 1.
  xTaskCreatePinnedToCore(&OneBitAudioOutput::audioTask, "audio", kAudioTaskStack, nullptr,
                          kAudioTaskPriority, &gTask, kAudioTaskCore);
}

void OneBitAudioOutput::fillRing() {
  int16_t block[kBlockSamples];
  // Писатель тут один (эта задача), поэтому голову держим в локальной
  // переменной; перечитывать надо только хвост — его двигает прерывание.
  uint32_t head = gHead;
  while ((uint32_t)(head - gTail) <= (uint32_t)(kRingSamples - kBlockSamples)) {
    gEngine->renderBlock(block, kBlockSamples);
    for (uint16_t i = 0; i < kBlockSamples; i++) {
      gRing[(head + i) & kRingMask] = block[i];
    }
    // Барьер: данные блока должны быть видны прерыванию (оно живёт на
    // другом ядре) раньше, чем сдвинется голова.
    __sync_synchronize();
    head += kBlockSamples;
    gHead = head;
  }
}

void OneBitAudioOutput::audioTask(void* arg) {
  (void)arg;

  gTimer = timerBegin(kTimerResolutionHz);
  timerAttachInterrupt(gTimer, &OneBitAudioOutput::onTimer);

  // Кольцо заполняется до запуска таймера, иначе первые сэмплы уйдут в
  // недобор и underruns() покажет отказ на пустом месте.
  fillRing();
  timerAlarm(gTimer, kTimerTicksPerSample, true, 0);

  for (;;) {
    ulTaskNotifyTake(pdTRUE, kFillTimeoutTicks);
    fillRing();
  }
}

void IRAM_ATTR OneBitAudioOutput::onTimer() {
  // Никакого рендера: только достать готовый сэмпл из DRAM и выставить пин.
  int16_t s = 0;
  const uint32_t tail = gTail;
  if (tail != gHead) {
    s = gRing[tail & kRingMask];
    gTail = tail + 1;
  } else {
    gUnderruns = gUnderruns + 1;  // задача не успела — отдаём тишину
  }
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

  if (gHigh != gWasHigh) {
    gEdges = gEdges + 1;
    gWasHigh = gHigh;
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

  if (++gRefillCount >= kBlockSamples) {
    gRefillCount = 0;
    if (gTask != nullptr) {
      BaseType_t higherPriorityWoken = pdFALSE;
      vTaskNotifyGiveFromISR(gTask, &higherPriorityWoken);
      if (higherPriorityWoken == pdTRUE) portYIELD_FROM_ISR();
    }
  }
}
