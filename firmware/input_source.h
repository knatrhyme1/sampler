// Общий интерфейс источника нот/CC: секвенсор работает одинаково,
// приходят ли события с виртуального контроллера в Wokwi или с
// настоящего MPK mini по USB-MIDI на железе (стори 3.1).
#pragma once

#include <stdint.h>

// CC-номера кнопок самого устройства (не MPK mini): у контроллера нет
// физических Play/Stop/Mode/Power, поэтому это отдельная договорённость,
// а не часть раскладки из mpk_mapping.h.
#define DEVICE_CC_PLAY_STOP 118
#define DEVICE_CC_MODE      119
#define DEVICE_CC_POWER     120

enum class InputEventType : uint8_t {
  NoteOn,
  NoteOff,
  ControlChange,
  // Поворот бесконечной крутилки: не абсолютное положение, а сколько
  // щелчков и в какую сторону её провернули с прошлого события. Параметр,
  // к которому крутилка сейчас привязана, меняется относительно своего
  // текущего значения, поэтому при смене экрана или параметра ничего не
  // прыгает. number — CC крутилки (MPK_KNOB_CC_BASE + индекс).
  KnobTurn,
};

struct InputEvent {
  InputEventType type;
  uint8_t channel;  // MIDI-канал, 1-based
  uint8_t number;   // номер ноты (NoteOn/NoteOff) или CC (ControlChange, KnobTurn)
  uint8_t value;    // сила нажатия (NoteOn) или значение CC (0-127)
  int8_t delta;     // KnobTurn: щелчки, > 0 — по часовой стрелке
};

class InputSource {
 public:
  virtual ~InputSource() = default;
  virtual void begin() = 0;
  // Возвращает true и заполняет ev, если есть новое событие.
  virtual bool poll(InputEvent& ev) = 0;
};
