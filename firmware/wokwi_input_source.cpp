#include "wokwi_input_source.h"

#include <soc/gpio_struct.h>

#include "mpk_mapping.h"

namespace {
// Энкодеры KY-040 (enc1..enc8 в diagram.json): CLK и DT каждой крутилки.
// Пины подобраны так, чтобы не задеть USB (19/20 — под MPK на железе),
// UART0 (43/44), октальную PSRAM (33–37) и strapping-пины 0/45/46.
constexpr uint8_t kKnobClk[WokwiInputSource::kNumKnobs] = {1, 4, 6, 8, 38, 40, 42, 48};
// DT читает прерывание — таблица лежит в RAM, а не во флеше.
const uint8_t DRAM_ATTR kKnobDt[WokwiInputSource::kNumKnobs] = {2, 5, 7, 9, 39, 41, 47, 3};

// Сколько щелчков накопила крутилка с прошлого опроса. Пишет прерывание,
// забирает задача опроса.
volatile int16_t gKnobCount[WokwiInputSource::kNumKnobs] = {};
portMUX_TYPE gKnobMux = portMUX_INITIALIZER_UNLOCKED;

constexpr uint32_t kScanPeriodMs = 2;
// Выше loop() (приоритет 1), ниже задачи звука (10, ядро 0).
constexpr UBaseType_t kInputTaskPriority = 3;
constexpr uint32_t kInputTaskStack = 3072;

inline bool IRAM_ATTR readPinFast(uint8_t pin) {
  return pin < 32 ? ((GPIO.in >> pin) & 1) : ((GPIO.in1.val >> (pin - 32)) & 1);
}

// Спад CLK — щелчок. Уровень DT в этот момент даёт направление: HIGH — по
// часовой, LOW — против (так KY-040 ведёт себя в Wokwi).
void IRAM_ATTR onKnobClk(void* arg) {
  const uint8_t i = (uint8_t)(uintptr_t)arg;
  const int16_t step = readPinFast(kKnobDt[i]) ? 1 : -1;
  portENTER_CRITICAL_ISR(&gKnobMux);
  gKnobCount[i] += step;
  portEXIT_CRITICAL_ISR(&gKnobMux);
}
}  // namespace

void WokwiInputSource::begin() {
  pinMode(pinLoad_, OUTPUT);
  pinMode(pinClock_, OUTPUT);
  pinMode(pinData_, INPUT);
  digitalWrite(pinLoad_, HIGH);
  digitalWrite(pinClock_, LOW);
  lastBits_ = readShiftRegisters();

  for (uint8_t i = 0; i < kNumKnobs; i++) {
    pinMode(kKnobClk[i], INPUT_PULLUP);
    pinMode(kKnobDt[i], INPUT_PULLUP);
    attachInterruptArg(kKnobClk[i], onKnobClk, (void*)(uintptr_t)i, FALLING);
  }

  queue_ = xQueueCreate(kQueueLength, sizeof(InputEvent));
  // На ядре 1, рядом с loop(): так задача вытесняет именно его, а не звук.
  xTaskCreatePinnedToCore(&WokwiInputSource::taskEntry, "input", kInputTaskStack, this,
                          kInputTaskPriority, nullptr, 1);
}

void WokwiInputSource::taskEntry(void* self) {
  WokwiInputSource* src = static_cast<WokwiInputSource*>(self);
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    src->scan();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(kScanPeriodMs));
  }
}

void WokwiInputSource::push(const InputEvent& ev) {
  // Очередь переполняется, только если loop() стоит больше секунды; тогда
  // новое событие теряется, а не блокирует опрос.
  xQueueSend(queue_, &ev, 0);
}

uint64_t WokwiInputSource::readShiftRegisters() {
  // Защёлкиваем состояние входов (PL низкий -> высокий).
  digitalWrite(pinLoad_, LOW);
  delayMicroseconds(5);
  digitalWrite(pinLoad_, HIGH);
  delayMicroseconds(5);

  // Сразу после защёлки на Q7 последней в цепи микросхемы уже стоит её D7,
  // без начального импульса CP. Дальше каждый импульс сдвигает следующий бит.
  uint64_t value = 0;
  for (uint8_t i = 0; i < kNumBits; i++) {
    value = (value << 1) | digitalRead(pinData_);
    digitalWrite(pinClock_, HIGH);
    delayMicroseconds(2);
    digitalWrite(pinClock_, LOW);
    delayMicroseconds(2);
  }
  return value;
}

bool WokwiInputSource::bitToEvent(uint8_t srNum, uint8_t dBit, bool pressed,
                                   InputEvent& ev) {
  if (srNum == 1) {
    // padN подключён к D(N-1) — шлём ту же ноту, что настоящий PADN на MPK.
    ev.type = pressed ? InputEventType::NoteOn : InputEventType::NoteOff;
    ev.channel = MPK_CHANNEL_PADS;
    ev.number = MPK_PAD_NOTES_BANK_A[dBit];
    ev.value = pressed ? 127 : 0;
    return true;
  }
  if (srNum >= 2 && srNum <= 4) {
    // Клавиши 1..24 подряд от MPK_KEY_LOWEST (C3).
    const uint8_t keyIndex = (srNum - 2) * 8 + dBit;  // 0..23
    ev.type = pressed ? InputEventType::NoteOn : InputEventType::NoteOff;
    ev.channel = MPK_CHANNEL_KEYS;
    ev.number = MPK_KEY_LOWEST + keyIndex;
    ev.value = pressed ? 100 : 0;
    return true;
  }
  // srNum == 5
  if (dBit == 0) {
    // 25-я, последняя клавиша (C5 == MPK_KEY_HIGHEST).
    ev.type = pressed ? InputEventType::NoteOn : InputEventType::NoteOff;
    ev.channel = MPK_CHANNEL_KEYS;
    ev.number = MPK_KEY_HIGHEST;
    ev.value = pressed ? 100 : 0;
    return true;
  }
  if (dBit == 1 || dBit == 2) {
    // У настоящего MPK mini нет кнопок Play/Stop, поэтому это не ноты, а
    // отдельные CC устройства (input_source.h).
    ev.type = InputEventType::ControlChange;
    ev.channel = MPK_CHANNEL_KEYS;
    ev.number = (dBit == 1) ? DEVICE_CC_PLAY_STOP : DEVICE_CC_MODE;
    ev.value = pressed ? 127 : 0;
    return true;
  }
  if (dBit == 3) {
    // Кнопка включения/выключения — тоже не с MPK, своя.
    ev.type = InputEventType::ControlChange;
    ev.channel = MPK_CHANNEL_KEYS;
    ev.number = DEVICE_CC_POWER;
    ev.value = pressed ? 127 : 0;
    return true;
  }
  return false;  // sr5.D4..D7 не используются
}

void WokwiInputSource::scan() {
  const uint64_t bits = readShiftRegisters();
  const uint64_t changed = bits ^ lastBits_;
  if (changed != 0) {
    // Все изменившиеся входы за один проход: одновременные нажатия не
    // откладываются на следующий опрос.
    for (int8_t i = kNumBits - 1; i >= 0; i--) {
      const uint64_t mask = (uint64_t)1 << i;
      if (!(changed & mask)) continue;
      // Дребезг: кнопка (и в Wokwi тоже — он его имитирует) несколько
      // миллисекунд после перепада скачет между 0 и 1, и опрос раз в 2 мс
      // принял бы скачки за отдельные нажатия. Поэтому после принятого
      // перепада вход 20 мс не слушаем. Если за это время кнопку уже
      // отпустили, отпускание примется сразу после паузы — сравнение с
      // принятым состоянием его не потеряет.
      if (lockout_[i] > 0) continue;
      lastBits_ ^= mask;
      lockout_[i] = kDebounceScans;

      // Кнопка тянет вход в LOW (пул-ап резистор к VCC на неотжатом входе).
      const bool pressed = (bits & mask) == 0;

      // i — позиция бита в 40-битном слове (0 — младший, т.е. прочитанный
      // последним). Переводим её в (номер микросхемы, номер входа D0..D7),
      // зная порядок цепи: esp -> sr5 -> sr4 -> sr3 -> sr2 -> sr1.
      const uint8_t k = kNumBits - 1 - i;         // 0 — бит, прочитанный первым
      const uint8_t chipOffset = k / 8;           // 0 для sr5, ..., 4 для sr1
      const uint8_t srNum = kNumShiftRegisters - chipOffset;
      const uint8_t dBit = 7 - (k % 8);

      InputEvent ev = {};
      if (bitToEvent(srNum, dBit, pressed, ev)) push(ev);
    }
  }
  for (uint8_t i = 0; i < kNumBits; i++) {
    if (lockout_[i] > 0) lockout_[i]--;
  }

  for (uint8_t i = 0; i < kNumKnobs; i++) {
    portENTER_CRITICAL(&gKnobMux);
    const int16_t count = gKnobCount[i];
    gKnobCount[i] = 0;
    portEXIT_CRITICAL(&gKnobMux);
    if (count == 0) continue;
    InputEvent ev = {};
    ev.type = InputEventType::KnobTurn;
    ev.channel = MPK_CHANNEL_KNOBS;
    ev.number = MPK_KNOB_CC_BASE + i;
    ev.delta = (int8_t)constrain(count, -127, 127);
    push(ev);
  }
}

bool WokwiInputSource::poll(InputEvent& ev) {
  return queue_ != nullptr && xQueueReceive(queue_, &ev, 0) == pdTRUE;
}
