// Читает виртуальный контроллер в Wokwi: пэды, клавиши и кнопки устройства —
// через цепочку из пяти 74HC165 (сдвиговые регистры), крутилки K1–K8 —
// бесконечные энкодеры KY-040 на прерываниях GPIO.
// Раскладка пинов и порядок бит соответствуют firmware/diagram.json.
//
// Опрос идёт в отдельной задаче, а не в loop(): полная перерисовка экрана
// держит loop() сотни миллисекунд, и нажатие, которое начиналось и
// заканчивалось за это время, раньше терялось целиком
// (docs/known-issues.md, п. 4). Задача с более высоким приоритетом
// вытесняет loop() прямо посреди отрисовки, снимает состояние кнопок и
// складывает события в очередь; loop() разбирает её, когда освободится.
#pragma once

#include <Arduino.h>

#include "input_source.h"

class WokwiInputSource : public InputSource {
 public:
  static const uint8_t kNumKnobs = 8;

  void begin() override;
  bool poll(InputEvent& ev) override;

 private:
  static const uint8_t kNumShiftRegisters = 5;
  static const uint8_t kNumBits = kNumShiftRegisters * 8;
  static const uint16_t kQueueLength = 64;

  const uint8_t pinLoad_ = 15;
  const uint8_t pinClock_ = 16;
  const uint8_t pinData_ = 17;

  // Подавление дребезга: принятое состояние входов и сколько ещё опросов
  // вход после принятого перепада не слушаем.
  static const uint8_t kDebounceScans = 10;  // 10 x 2 мс = 20 мс
  uint64_t lastBits_ = 0;
  uint8_t lockout_[kNumBits] = {};
  QueueHandle_t queue_ = nullptr;

  static void taskEntry(void* self);
  void scan();
  void push(const InputEvent& ev);
  uint64_t readShiftRegisters();
  // srNum: 1..5 (номер микросхемы в цепи, sr1 — самая дальняя от платы).
  // dBit: 0..7 (номер входа D0..D7 на этой микросхеме).
  bool bitToEvent(uint8_t srNum, uint8_t dBit, bool pressed, InputEvent& ev);
};
