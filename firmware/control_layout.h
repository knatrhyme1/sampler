// Навигационный кластер (docs/poc-backlog-draft.md, B.2/B.4): пэды банка A
// несут фиксированные роли. Прошивка видит только MIDI-ноты с контроллера
// (MPK по USB или его эмуляцию в Wokwi), поэтому роль определяется цепочкой
// нота -> номер пэда на корпусе Akai -> команда.
//
// Раскладка на корпусе:
//   верхний ряд: PAD5 PAD6 PAD7 PAD8
//   нижний ряд:  PAD1 PAD2 PAD3 PAD4
#pragma once

#include <stdint.h>

#include "mpk_mapping.h"

// Какой из смыслов PAD1/PAD3 (иерархия или влево/вправо) сейчас активен,
// решает конкретный экран.
enum class NavCommand : uint8_t {
  None,
  Up,        // PAD6
  Down,      // PAD2
  Back,      // PAD1 — назад / уровень выше / влево
  Forward,   // PAD3 — вперёд / глубже / вправо
  Confirm,   // PAD5
  Cancel,    // PAD7
  ContextA,  // PAD8
  ContextB,  // PAD4
};

// 1..8 — номер пэда банка A на корпусе, 0 — нота не с пэда банка A.
inline uint8_t padNumberForNote(uint8_t channel, uint8_t note) {
  if (channel != MPK_CHANNEL_PADS) return 0;
  for (uint8_t i = 0; i < 8; i++) {
    if (MPK_PAD_NOTES_BANK_A[i] == note) return i + 1;
  }
  return 0;
}

inline NavCommand navCommandForPad(uint8_t padNumber) {
  switch (padNumber) {
    case 1: return NavCommand::Back;
    case 2: return NavCommand::Down;
    case 3: return NavCommand::Forward;
    case 4: return NavCommand::ContextB;
    case 5: return NavCommand::Confirm;
    case 6: return NavCommand::Up;
    case 7: return NavCommand::Cancel;
    case 8: return NavCommand::ContextA;
    default: return NavCommand::None;
  }
}

inline NavCommand navCommandForNoteOn(uint8_t channel, uint8_t note) {
  return navCommandForPad(padNumberForNote(channel, note));
}
