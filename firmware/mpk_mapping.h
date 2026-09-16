// Раскладка Akai MPK mini mk3, снятая живьём с реального контроллера.
// Полное описание, включая кнопки без собственного MIDI-сигнала и найденные
// особенности поведения — в docs/mpk-mapping.md. Номера пэдов зависят от
// текущей PGM на контроллере (сейчас — кастомная под FL Studio); при смене
// PGM переснять через tools/midi_logger.py и обновить это файл.
#pragma once

#include <stdint.h>

// MIDI-каналы, как в спецификации (1-based)
#define MPK_CHANNEL_KEYS     1
#define MPK_CHANNEL_PADS     10
#define MPK_CHANNEL_KNOBS    1
#define MPK_CHANNEL_JOYSTICK 1

// Клавиши: 25 шт, хроматика, при Octave = 0 на контроллере
#define MPK_KEY_LOWEST  48  // C3
#define MPK_KEY_HIGHEST 72  // C5

// Пэды: индекс 0-3 — верхний ряд слева направо, 4-7 — нижний ряд слева направо
static const uint8_t MPK_PAD_NOTES_BANK_A[8] = {52, 53, 55, 57, 45, 47, 48, 50};
static const uint8_t MPK_PAD_NOTES_BANK_B[8] = {65, 67, 69, 71, 59, 60, 62, 64};

// Крутилки K1..K8 -> CC (MPK_KNOB_CC_BASE + индекс 0..7)
#define MPK_KNOB_CC_BASE 70

// Джойстик: X — Pitch Bend на MPK_CHANNEL_JOYSTICK, Y — CC ниже (mod wheel)
#define MPK_JOYSTICK_Y_CC 1
