// SMPLR — DAWless сэмплер-секвенсор на ESP32-S3 (PoC, живёт в Wokwi).
//
// Здесь — машина состояний устройства: питание, экраны и что делает каждый
// элемент управления на каждом экране. Рисование — ui_screens.*, звук —
// audio_engine.* / transport.*, ввод — InputSource (в Wokwi его реализует
// WokwiInputSource, на железе заменит чтение MPK mini по USB-MIDI, стори 3.3).
//
// Общие правила управления (docs/archive/poc-backlog-draft.md, B.2/B.4):
//   PAD6/PAD2/PAD1/PAD3 — курсор вверх/вниз/влево/вправо;
//   PAD5 — подтвердить (нажать элемент под курсором, поставить шаг);
//   PAD7 — назад, на любом экране;
//   PAD4 — метроном вкл/выкл с любого экрана; PAD8 — действие экрана;
//   MODE — меню и обратно туда, откуда пришли; PLAY/STOP — транспорт;
//   крутилки бесконечные: меняют параметр, к которому привязаны на этом
//   экране, относительно его текущего значения — ничего не прыгает.

#include "board_config.h"
#include "wokwi_input_source.h"
#include "ui_screens.h"
#include "control_layout.h"
#include "step_sequencer.h"
#include "piano_roll.h"
#include "transport.h"
#include "audio_engine.h"
#include "audio_output_1bit.h"
#include "default_samples.h"
#include "pattern_export.h"

const uint32_t SERIAL_BAUD = 115200;
const unsigned long BEAT_INTERVAL_MS = 1000;
const unsigned long POWER_HOLD_MS = 600;  // защита от случайного нажатия
const unsigned long BOOT_DURATION_MS = 3000;
const unsigned long BOOT_FRAME_MS = 60;  // ~16 fps перерисовки глитч-анимации
const unsigned long CLEAR_CONFIRM_MS = 3000;
const uint16_t DEFAULT_BPM = 120;
const uint16_t BPM_MIN = 40;
const uint16_t BPM_MAX = 240;
const uint8_t DEFAULT_SECTION = kSectionSequencer;
const uint8_t MIXER_PAD_STEP = 5;  // PAD6/PAD2 на микшере: громкость +-5
// Скорость крутилок (меню INPUT): на сколько единиц меняется значение за
// один щелчок.
const uint8_t KNOB_SPEEDS[] = {1, 2, 4};
const uint8_t KNOB_SPEED_COUNT = sizeof(KNOB_SPEEDS) / sizeof(KNOB_SPEEDS[0]);

// Звуковой выход мастер-шины: 1-битный поток на GPIO21, к нему в
// diagram.json подключена пищалка bz1 (см. audio_output_1bit.h).
const uint8_t AUDIO_PIN = 21;
const unsigned long METER_REDRAW_INTERVAL_MS = 30;

enum class PowerState : uint8_t { Off, Boot, Home };

// Экраны внутри PowerState::Home: главный экран, разделы проекта 1–4 и
// меню (список пунктов / открытый пункт).
enum class UiMode : uint8_t {
  HomeMain,
  Sequencer,    // раздел 1
  Mixer,        // раздел 2
  PianoRoll,    // раздел 3
  Arrangement,  // раздел 4
  MenuList,
  MenuItem,
};

WokwiInputSource inputSource;
UiScreens ui;
Transport transport;
AudioEngine audio;
OneBitAudioOutput audioOutput;
PatternExporter exporter;
MixerSettings mixer;
PianoRoll pianoRoll;

// Ваншоты по умолчанию для каналов KICK/SNARE/HAT/PERC (ACIDKICK DRUM KIT,
// сконвертированы tools/wav_to_header.py в default_samples.h, 44,1 кГц —
// живой вывод пересчитывает их в свои 16 кГц, экспорт берёт как есть).
const OneShot kDefaultOneShots[StepSequencer::kTracks] = {
    {kSampleKick, kSampleKickLength, kDefaultSamplesRate},
    {kSampleSnare, kSampleSnareLength, kDefaultSamplesRate},
    {kSampleHat, kSampleHatLength, kDefaultSamplesRate},
    {kSamplePerc, kSamplePercLength, kDefaultSamplesRate},
};

unsigned long lastBeat = 0;
PowerState powerState = PowerState::Off;
UiMode uiMode = UiMode::HomeMain;
// Куда вернуться из меню по MODE / PAD7.
UiMode menuReturnMode = UiMode::HomeMain;
uint8_t activeSection = DEFAULT_SECTION;
// Курсор главного экрана только показывает, на чём стоит селектор; выбор
// (нажатие) делает PAD5. activeSection меняется только нажатием.
HomeFocus homeFocus = HomeFocus::Sections;
uint8_t sectionCursor = DEFAULT_SECTION;
uint16_t currentBpm = DEFAULT_BPM;
uint8_t menuSelected = 0;
uint8_t menuOpenItem = 0;
uint8_t tempoRow = 0;
uint8_t knobSpeedIndex = 0;
uint8_t lastKnob = 255;
int8_t lastKnobDelta = 0;
// Последнее абсолютное значение крутилки, если контроллер шлёт обычный CC
// (MPK с заводской настройкой): из него считается приращение.
int16_t knobAbsLast[WokwiInputSource::kNumKnobs] = {-1, -1, -1, -1, -1, -1, -1, -1};

// Настройка "метроном участвует в проигрывании". Сама по себе ничего не
// запускает: щелчки идут только пока транспорт играет (PLAY), на каждой
// доле по часам секвенсора.
bool metronomeOn = false;

// Раздел 1 — степ-секвенсор.
uint8_t seqCursorTrack = 0;
uint8_t seqCursorStep = 0;
uint8_t clearConfirmTrack = 255;  // канал, для которого ждём второе PAD8
unsigned long clearConfirmAt = 0;
// Раздел 2 — микшер.
uint8_t mixerCursor = 0;
// Раздел 3 — пиано-ролл.
uint8_t rollStep = 0;
uint8_t rollPitch = 12;  // C4 — середина двух октав
uint8_t rollViewBottom = 6;
// Раздел 4 — аранжировка.
uint8_t arrRow = 0;
uint8_t arrBar = 0;

// Сколько раз паттерн прошёл по кругу с последнего PLAY — нужно
// аранжировке, чтобы бегущий такт шёл по шкале, а не стоял в первом блоке.
uint16_t loopCount = 0;
uint8_t lastPlayhead = Transport::kPlayheadIdle;

// Экспорт закончился, пока пользователь был на другом экране: на главном
// висит отметка EXPORT DONE, пока страницу EXPORT не откроют.
bool exportUnseen = false;
// Пользователь пытался экспортировать пустой паттерн.
bool exportEmptyShown = false;

float soundMeterLevel = 0.0f;
float mixerMeterLevels[MixerSettings::kStrips] = {};
unsigned long lastMeterDrawAt = 0;

bool powerButtonDown = false;
unsigned long powerButtonDownAt = 0;
bool powerActionTriggered = false;

unsigned long bootStartedAt = 0;
unsigned long bootFrameShownAt = 0;

bool onHome() { return powerState == PowerState::Home; }

bool exportRunning() { return exporter.state() == PatternExporter::State::Running; }

// ---------------------------------------------------------------------------
// Отрисовка.

HomeView homeView() {
  HomeView v = {};
  v.bpm = currentBpm;
  v.activeSection = activeSection;
  v.sectionCursor = sectionCursor;
  v.focus = homeFocus;
  v.metronomeOn = metronomeOn;
  v.playing = transport.playing();
  const uint8_t ph = transport.playhead();
  v.bar = ph == Transport::kPlayheadIdle ? 0 : ph / StepSequencer::kStepsPerBar;
  v.silent = !metronomeOn && transport.pattern().empty();
  switch (exporter.state()) {
    case PatternExporter::State::Running:
      v.exportStatus = ExportStatus::Running;
      break;
    case PatternExporter::State::Done:
      v.exportStatus = ExportStatus::Done;
      break;
    case PatternExporter::State::Aborted:
      v.exportStatus = ExportStatus::Aborted;
      break;
    default:
      v.exportStatus = ExportStatus::Ready;
      break;
  }
  v.exportPercent = exporter.percent();
  v.exportUnseen = exportUnseen;
  return v;
}

// Пока экспорт не запускали, страница показывает файл для текущего
// паттерна и темпа; после запуска — то, что ушло или уходит, с темпом
// экспорта, а не текущим (его могли поменять, пока экспорт шёл).
void renderExportPage() {
  ExportView v = {};
  const PatternExporter::State st = exporter.state();
  const bool fresh = st == PatternExporter::State::Idle || exportEmptyShown;
  if (fresh) {
    const PatternExporter::Plan plan =
        PatternExporter::plan(transport.pattern(), currentBpm, kDefaultOneShots);
    v.status = exportEmptyShown ? ExportStatus::Empty : ExportStatus::Ready;
    v.bpm = currentBpm;
    v.durationMs = plan.durationMs;
    v.fileBytes = plan.fileBytes;
  } else {
    const PatternExporter::Plan& plan = exporter.currentPlan();
    v.status = st == PatternExporter::State::Running ? ExportStatus::Running
               : st == PatternExporter::State::Done  ? ExportStatus::Done
                                                     : ExportStatus::Aborted;
    v.bpm = exporter.bpm();
    v.durationMs = plan.durationMs;
    v.fileBytes = plan.fileBytes;
  }
  v.percent = exporter.percent();
  v.fileName = exporter.fileName();
  ui.showExport(v);
}

void renderMenuItem() {
  switch (menuOpenItem) {
    case kMenuItemTempo:
      ui.showTempo(currentBpm, metronomeOn, tempoRow);
      break;
    case kMenuItemInput:
      ui.showInput(KNOB_SPEEDS[knobSpeedIndex], lastKnob, lastKnobDelta);
      break;
    case kMenuItemSystem: {
      SystemInfo info = {};
      info.flashKb = ESP.getFlashChipSize() / 1024;
      info.psramKb = ESP.getPsramSize() / 1024;
      info.heapFreeKb = ESP.getFreeHeap() / 1024;
      info.uptimeS = millis() / 1000;
      ui.showSystem(info);
      break;
    }
    case kMenuItemExport:
      renderExportPage();
      break;
    default:
      ui.showMenuStub(menuOpenItem);
      break;
  }
}

uint8_t arrangementPlayBar() {
  const uint8_t ph = transport.playhead();
  if (ph == Transport::kPlayheadIdle) return 255;
  const uint8_t patternBars = StepSequencer::kSteps / StepSequencer::kStepsPerBar;
  return (uint8_t)((loopCount % 4) * patternBars + ph / StepSequencer::kStepsPerBar);
}

void renderUiMode() {
  switch (uiMode) {
    case UiMode::HomeMain:
      ui.showHome(homeView());
      break;
    case UiMode::Sequencer:
      ui.showSequencer(transport.pattern(), transport.playing(), transport.playhead(),
                       seqCursorTrack, seqCursorStep, currentBpm, metronomeOn);
      if (clearConfirmTrack != 255) ui.updateSequencerFooter(clearConfirmTrack);
      break;
    case UiMode::Mixer:
      ui.showMixer(mixer, mixerCursor);
      break;
    case UiMode::PianoRoll:
      ui.showPianoRoll(pianoRoll, rollStep, rollPitch, rollViewBottom, transport.playhead());
      break;
    case UiMode::Arrangement:
      ui.showArrangement(transport.pattern(), pianoRoll, arrRow, arrBar, arrangementPlayBar());
      break;
    case UiMode::MenuList:
      ui.showMenuList(menuSelected);
      break;
    case UiMode::MenuItem:
      renderMenuItem();
      break;
  }
}

// Главный экран перерисовывает только изменившееся, поэтому его можно
// дёргать после любого изменения состояния.
void refreshHome() {
  if (onHome() && uiMode == UiMode::HomeMain) ui.showHome(homeView());
}

void setMode(UiMode mode) {
  uiMode = mode;
  renderUiMode();
}

// ---------------------------------------------------------------------------
// Общие параметры: темп, метроном, микшер.

void setBpm(int32_t bpm) {
  bpm = constrain(bpm, (int32_t)BPM_MIN, (int32_t)BPM_MAX);
  if ((uint16_t)bpm == currentBpm) return;
  currentBpm = (uint16_t)bpm;
  transport.setBpm(currentBpm);
  if (uiMode == UiMode::Sequencer) {
    ui.updateSequencerHeader(transport.playing(), currentBpm, metronomeOn);
  } else if (uiMode == UiMode::MenuItem && menuOpenItem == kMenuItemTempo) {
    ui.showTempo(currentBpm, metronomeOn, tempoRow);
  }
  refreshHome();
}

// Кнопка MET только решает, будет ли метроном щёлкать вместе с остальным при
// проигрывании. Если транспорт уже играет, щелчки начнутся/прекратятся со
// следующей доли.
void setMetronomeEnabled(bool on) {
  metronomeOn = on;
  transport.setMetronome(on);
  Serial.printf("metronome: %s\n", on ? "on" : "off");
  if (uiMode == UiMode::Sequencer) {
    ui.updateSequencerHeader(transport.playing(), currentBpm, metronomeOn);
  } else if (uiMode == UiMode::MenuItem && menuOpenItem == kMenuItemTempo) {
    ui.showTempo(currentBpm, metronomeOn, tempoRow);
  }
  refreshHome();
}

void setMixerVolume(uint8_t strip, int32_t volume) {
  volume = constrain(volume, 0, (int32_t)MixerSettings::kMaxVolume);
  if (mixer.volume[strip] == (uint8_t)volume) return;
  mixer.volume[strip] = (uint8_t)volume;
  audio.applyMixer(mixer);
  if (uiMode == UiMode::Mixer) ui.updateMixerStrip(mixer, strip, strip == mixerCursor);
}

// ---------------------------------------------------------------------------
// Питание.

void enterOff() {
  exporter.abort();
  transport.stop();
  audio.stopAll();
  // Выключенное устройство молчит целиком: вывод заглушён до enterHome().
  audioOutput.setMuted(true);
  soundMeterLevel = 0.0f;
  clearConfirmTrack = 255;
  exportEmptyShown = false;
  powerState = PowerState::Off;
  uiMode = UiMode::HomeMain;
  menuReturnMode = UiMode::HomeMain;
  ui.showOff();
  Serial.println("power: off");
}

void enterBoot() {
  powerState = PowerState::Boot;
  bootStartedAt = millis();
  bootFrameShownAt = 0;
  Serial.println("power: boot");
}

void enterHome() {
  powerState = PowerState::Home;
  uiMode = UiMode::HomeMain;
  audioOutput.setMuted(false);
  renderUiMode();
  Serial.println("power: home/ready");
}

void updateBoot() {
  const unsigned long elapsed = millis() - bootStartedAt;
  if (elapsed >= BOOT_DURATION_MS) {
    enterHome();
    return;
  }
  if (millis() - bootFrameShownAt >= BOOT_FRAME_MS) {
    bootFrameShownAt = millis();
    const uint8_t percent = (uint8_t)((elapsed * 100) / BOOT_DURATION_MS);
    ui.showBoot(percent);
  }
}

void handlePowerButton(const InputEvent& ev) {
  if (ev.type != InputEventType::ControlChange || ev.number != DEVICE_CC_POWER) return;
  if (ev.value > 0) {
    powerButtonDown = true;
    powerButtonDownAt = millis();
    powerActionTriggered = false;
  } else {
    powerButtonDown = false;
    if (powerState == PowerState::Off) ui.updatePowerHold(0);  // отпустили раньше времени
  }
}

// Долгое удержание POWER (>= POWER_HOLD_MS) переключает питание. На экране
// "выключено" полоса показывает, сколько ещё держать. Во время загрузки
// кнопка игнорируется — процесс нельзя прервать.
void updatePowerButton(unsigned long now) {
  if (!powerButtonDown || powerActionTriggered) return;
  const unsigned long held = now - powerButtonDownAt;
  if (held < POWER_HOLD_MS) {
    if (powerState == PowerState::Off) ui.updatePowerHold((uint8_t)(held * 100 / POWER_HOLD_MS));
    return;
  }
  powerActionTriggered = true;
  if (powerState == PowerState::Off) {
    ui.updatePowerHold(100);
    enterBoot();
  } else if (powerState == PowerState::Home) {
    enterOff();
  }
}

// ---------------------------------------------------------------------------
// Кнопки устройства: MODE и PLAY/STOP.

// MODE открывает меню с любого экрана и возвращает туда, откуда пришли.
// Удержание (SHIFT) — открытый вопрос B.4, пока не реализовано.
void handleModeButton(const InputEvent& ev) {
  if (ev.type != InputEventType::ControlChange || ev.number != DEVICE_CC_MODE) return;
  if (ev.value == 0) return;  // реагируем на нажатие, не на отпускание
  if (!onHome()) return;

  if (uiMode == UiMode::MenuList || uiMode == UiMode::MenuItem) {
    exportEmptyShown = false;
    setMode(menuReturnMode);
  } else {
    menuReturnMode = uiMode;
    setMode(UiMode::MenuList);
  }
}

// PLAY/STOP (стори 1.2): единый транспорт проекта. Старт всегда с первого
// шага; переходы между экранами проигрывание не прерывают — остановить его
// можно только этой кнопкой (или выключением питания). Во время экспорта
// кнопка не действует: экспорт сам остановил проигрывание и занимает процессор.
void handlePlayStopButton(const InputEvent& ev) {
  if (ev.type != InputEventType::ControlChange || ev.number != DEVICE_CC_PLAY_STOP) return;
  if (ev.value == 0) return;
  if (!onHome()) return;
  if (exportRunning()) {
    Serial.println("seq: play ignored (export running)");
    return;
  }

  if (transport.playing()) {
    transport.stop();  // уже звучащие удары доигрывают свой хвост
    Serial.println("seq: stop");
  } else {
    loopCount = 0;
    lastPlayhead = Transport::kPlayheadIdle;
    transport.play();
    Serial.println("seq: play");
  }
  if (uiMode == UiMode::Sequencer) {
    ui.updateSequencerHeader(transport.playing(), currentBpm, metronomeOn);
  }
  refreshHome();
}

// ---------------------------------------------------------------------------
// Крутилки.

// Приводит поворот крутилки к приращению. Wokwi-энкодеры сразу шлют
// KnobTurn; обычный абсолютный CC (MPK с заводской настройкой) переводится
// в разницу с прошлым значением — так параметр всё равно не прыгает, хотя
// у абсолютной крутилки есть упор в 0 и 127.
bool knobDelta(const InputEvent& ev, uint8_t& knob, int16_t& delta) {
  if (ev.channel != MPK_CHANNEL_KNOBS || ev.number < MPK_KNOB_CC_BASE ||
      ev.number >= MPK_KNOB_CC_BASE + WokwiInputSource::kNumKnobs) {
    return false;
  }
  knob = ev.number - MPK_KNOB_CC_BASE;
  if (ev.type == InputEventType::KnobTurn) {
    delta = ev.delta;
    return delta != 0;
  }
  if (ev.type != InputEventType::ControlChange) return false;
  const int16_t last = knobAbsLast[knob];
  knobAbsLast[knob] = ev.value;
  if (last < 0) return false;  // первое значение — только запомнить
  delta = (int16_t)ev.value - last;
  return delta != 0;
}

// K1 — темп: на главном экране (где бы ни стоял курсор), на странице
// секвенсора и в TEMPO. K1–K6 на микшере — громкости полос.
void handleKnob(const InputEvent& ev) {
  uint8_t knob;
  int16_t delta;
  if (!onHome() || !knobDelta(ev, knob, delta)) return;

  lastKnob = knob;
  lastKnobDelta = (int8_t)constrain(delta, -127, 127);
  const int32_t step = (int32_t)delta * KNOB_SPEEDS[knobSpeedIndex];

  switch (uiMode) {
    case UiMode::HomeMain:
      if (knob == 0) setBpm((int32_t)currentBpm + step);
      break;
    case UiMode::Sequencer:
      if (knob == 0) setBpm((int32_t)currentBpm + step);
      break;
    case UiMode::Mixer:
      if (knob < MixerSettings::kStrips) {
        setMixerVolume(knob, (int32_t)mixer.volume[knob] + step);
      }
      break;
    case UiMode::MenuItem:
      if (menuOpenItem == kMenuItemTempo && knob == 0) {
        setBpm((int32_t)currentBpm + step);
      } else if (menuOpenItem == kMenuItemInput) {
        ui.showInput(KNOB_SPEEDS[knobSpeedIndex], lastKnob, lastKnobDelta);
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Клавиши.

// Нижние клавиши C3..D#3 (B.4) проигрывают ваншот канала 1–4 через ту же
// мастер-шину — прослушать звук, не запуская паттерн. На странице
// секвенсора клавиша ещё и переводит курсор на этот канал, на микшере —
// выбирает его полосу.
void handleChannelKey(const InputEvent& ev) {
  if (ev.type != InputEventType::NoteOn || ev.channel != MPK_CHANNEL_KEYS) return;
  if (ev.number < MPK_KEY_LOWEST || ev.number >= MPK_KEY_LOWEST + StepSequencer::kTracks) return;
  if (!onHome()) return;

  const uint8_t track = ev.number - MPK_KEY_LOWEST;
  audio.trigger(track, kDefaultOneShots[track]);
  if (uiMode == UiMode::Sequencer && track != seqCursorTrack) {
    const uint8_t oldTrack = seqCursorTrack;
    seqCursorTrack = track;
    ui.updateSequencerCursor(transport.pattern(), oldTrack, seqCursorStep, seqCursorTrack,
                             seqCursorStep);
  } else if (uiMode == UiMode::Mixer && track != mixerCursor) {
    const uint8_t old = mixerCursor;
    mixerCursor = track;
    ui.updateMixerStrip(mixer, old, false);
    ui.updateMixerStrip(mixer, mixerCursor, true);
  }
}

// ---------------------------------------------------------------------------
// Навигационный кластер: одни и те же команды ведут себя по-разному в
// зависимости от текущего экрана.

void openSection(uint8_t section) {
  switch (section) {
    case kSectionSequencer:
      setMode(UiMode::Sequencer);
      break;
    case kSectionMixer:
      setMode(UiMode::Mixer);
      break;
    case kSectionPianoRoll:
      setMode(UiMode::PianoRoll);
      break;
    default:
      setMode(UiMode::Arrangement);
      break;
  }
}

void navHome(NavCommand cmd) {
  // PAD6/PAD2 — курсор между верхней строкой (BPM, MET) и рядом разделов
  // проекта 1–4; PAD1/PAD3 — влево/вправо внутри строки (B.4). PAD5
  // нажимает элемент под курсором: MET — вкл/выкл метроном; раздел —
  // первое нажатие выбирает его, нажатие на уже выбранный открывает его
  // страницу. У BPM нажатия нет — темп крутится K1.
  if (cmd == NavCommand::Up && homeFocus == HomeFocus::Sections) {
    homeFocus = HomeFocus::Bpm;
  } else if (cmd == NavCommand::Down && homeFocus != HomeFocus::Sections) {
    homeFocus = HomeFocus::Sections;
    sectionCursor = activeSection;
  } else if (cmd == NavCommand::Forward && homeFocus == HomeFocus::Bpm) {
    homeFocus = HomeFocus::Metronome;
  } else if (cmd == NavCommand::Back && homeFocus == HomeFocus::Metronome) {
    homeFocus = HomeFocus::Bpm;
  } else if (cmd == NavCommand::Back && homeFocus == HomeFocus::Sections) {
    sectionCursor = (sectionCursor == 0) ? kSectionCount - 1 : sectionCursor - 1;
  } else if (cmd == NavCommand::Forward && homeFocus == HomeFocus::Sections) {
    sectionCursor = (sectionCursor + 1) % kSectionCount;
  } else if (cmd == NavCommand::Confirm && homeFocus == HomeFocus::Metronome) {
    setMetronomeEnabled(!metronomeOn);
    return;
  } else if (cmd == NavCommand::Confirm && homeFocus == HomeFocus::Sections) {
    if (sectionCursor != activeSection) {
      activeSection = sectionCursor;
    } else {
      openSection(activeSection);
      return;
    }
  } else {
    return;
  }
  refreshHome();
}

void cancelClearConfirm() {
  if (clearConfirmTrack == 255) return;
  clearConfirmTrack = 255;
  if (uiMode == UiMode::Sequencer) ui.updateSequencerFooter(255);
}

void navSequencer(NavCommand cmd) {
  // Сетка как таблица: PAD6/PAD2 — канал вверх/вниз, PAD1/PAD3 — шаг
  // влево/вправо (с переходом через край, чтобы с 16-го шага попасть на
  // 1-й одним нажатием). PAD5 ставит/убирает удар под курсором, PAD7 —
  // назад на главный экран, PAD8 — очистить канал (со вторым нажатием).
  if (cmd != NavCommand::ContextA) cancelClearConfirm();

  const uint8_t oldTrack = seqCursorTrack;
  const uint8_t oldStep = seqCursorStep;
  switch (cmd) {
    case NavCommand::Up:
      seqCursorTrack = (seqCursorTrack == 0) ? StepSequencer::kTracks - 1 : seqCursorTrack - 1;
      break;
    case NavCommand::Down:
      seqCursorTrack = (seqCursorTrack + 1) % StepSequencer::kTracks;
      break;
    case NavCommand::Back:
      seqCursorStep = (seqCursorStep == 0) ? StepSequencer::kSteps - 1 : seqCursorStep - 1;
      break;
    case NavCommand::Forward:
      seqCursorStep = (seqCursorStep + 1) % StepSequencer::kSteps;
      break;
    case NavCommand::Confirm:
      transport.toggleStep(seqCursorTrack, seqCursorStep);
      ui.updateSequencerCell(transport.pattern(), seqCursorTrack, seqCursorStep, seqCursorTrack,
                             seqCursorStep);
      return;
    case NavCommand::Cancel:
      // Проигрывание не прерывается.
      setMode(UiMode::HomeMain);
      return;
    case NavCommand::ContextA:
      if (clearConfirmTrack == seqCursorTrack) {
        transport.clearTrack(seqCursorTrack);
        Serial.printf("seq: clear track %u\n", (unsigned)(seqCursorTrack + 1));
        clearConfirmTrack = 255;
        renderUiMode();
      } else {
        clearConfirmTrack = seqCursorTrack;
        clearConfirmAt = millis();
        ui.updateSequencerFooter(clearConfirmTrack);
      }
      return;
    default:
      return;
  }
  ui.updateSequencerCursor(transport.pattern(), oldTrack, oldStep, seqCursorTrack, seqCursorStep);
}

void navMixer(NavCommand cmd) {
  // PAD1/PAD3 — выбор полосы, PAD6/PAD2 — громкость выбранной +-5, PAD5 —
  // mute, PAD7 — назад. Крутилки K1–K6 крутят громкость своей полосы
  // напрямую, без выбора.
  const uint8_t old = mixerCursor;
  switch (cmd) {
    case NavCommand::Back:
      mixerCursor = (mixerCursor == 0) ? MixerSettings::kStrips - 1 : mixerCursor - 1;
      break;
    case NavCommand::Forward:
      mixerCursor = (mixerCursor + 1) % MixerSettings::kStrips;
      break;
    case NavCommand::Up:
      setMixerVolume(mixerCursor, (int32_t)mixer.volume[mixerCursor] + MIXER_PAD_STEP);
      return;
    case NavCommand::Down:
      setMixerVolume(mixerCursor, (int32_t)mixer.volume[mixerCursor] - MIXER_PAD_STEP);
      return;
    case NavCommand::Confirm:
      mixer.muted[mixerCursor] = !mixer.muted[mixerCursor];
      audio.applyMixer(mixer);
      ui.updateMixerStrip(mixer, mixerCursor, true);
      return;
    case NavCommand::Cancel:
      setMode(UiMode::HomeMain);
      return;
    default:
      return;
  }
  ui.updateMixerStrip(mixer, old, false);
  ui.updateMixerStrip(mixer, mixerCursor, true);
}

void navPianoRoll(NavCommand cmd) {
  const uint8_t oldStep = rollStep;
  const uint8_t oldPitch = rollPitch;
  switch (cmd) {
    case NavCommand::Up:
      if (rollPitch + 1 < PianoRoll::kPitches) rollPitch++;
      break;
    case NavCommand::Down:
      if (rollPitch > 0) rollPitch--;
      break;
    case NavCommand::Back:
      rollStep = (rollStep == 0) ? PianoRoll::kSteps - 1 : rollStep - 1;
      break;
    case NavCommand::Forward:
      rollStep = (rollStep + 1) % PianoRoll::kSteps;
      break;
    case NavCommand::Confirm:
      pianoRoll.toggle(rollStep, rollPitch);
      ui.updatePianoCell(pianoRoll, rollStep, rollPitch, true);
      return;
    case NavCommand::Cancel:
      setMode(UiMode::HomeMain);
      return;
    default:
      return;
  }
  // Курсор ушёл за край окна — окно едет за ним (одна октава на экране).
  if (rollPitch < rollViewBottom) {
    rollViewBottom = rollPitch;
    renderUiMode();
    return;
  }
  if (rollPitch >= rollViewBottom + UiScreens::kPianoVisibleRows) {
    rollViewBottom = rollPitch - UiScreens::kPianoVisibleRows + 1;
    renderUiMode();
    return;
  }
  ui.updatePianoCell(pianoRoll, oldStep, oldPitch, false);
  ui.updatePianoCell(pianoRoll, rollStep, rollPitch, true);
  ui.updatePianoCursorLabel(rollStep, rollPitch);
}

void navArrangement(NavCommand cmd) {
  const uint8_t oldRow = arrRow;
  const uint8_t oldBar = arrBar;
  switch (cmd) {
    case NavCommand::Up:
      arrRow = (arrRow == 0) ? 4 : arrRow - 1;
      break;
    case NavCommand::Down:
      arrRow = (arrRow + 1) % 5;
      break;
    case NavCommand::Back:
      arrBar = (arrBar == 0) ? 15 : arrBar - 1;
      break;
    case NavCommand::Forward:
      arrBar = (arrBar + 1) % 16;
      break;
    case NavCommand::Cancel:
      setMode(UiMode::HomeMain);
      return;
    default:
      return;
  }
  ui.updateArrangementCursor(oldRow, oldBar, arrRow, arrBar, transport.pattern(), pianoRoll);
}

void navMenuList(NavCommand cmd) {
  if (cmd == NavCommand::Up) {
    menuSelected = (menuSelected == 0) ? kMenuItemCount - 1 : menuSelected - 1;
    ui.showMenuList(menuSelected);
  } else if (cmd == NavCommand::Down) {
    menuSelected = (menuSelected + 1) % kMenuItemCount;
    ui.showMenuList(menuSelected);
  } else if (cmd == NavCommand::Forward || cmd == NavCommand::Confirm) {
    menuOpenItem = menuSelected;
    if (menuOpenItem == kMenuItemExport) exportUnseen = false;  // результат увидели
    setMode(UiMode::MenuItem);
  } else if (cmd == NavCommand::Back || cmd == NavCommand::Cancel) {
    setMode(menuReturnMode);
  }
}

// Страница EXPORT: PAD5 — старт (транспорт при этом останавливается: экспорт
// рендерит паттерн сам, офлайн), PAD7 — отмена идущего экспорта или выход.
void navExport(NavCommand cmd) {
  if (exportRunning()) {
    if (cmd == NavCommand::Cancel) {
      exporter.abort();
      Serial.println("export: cancelled");
      renderUiMode();
    }
    return;
  }

  if (cmd == NavCommand::Confirm) {
    const StepSequencer pattern = transport.pattern();
    if (pattern.empty()) {
      exportEmptyShown = true;
      renderUiMode();
      return;
    }
    exportEmptyShown = false;
    if (transport.playing()) {
      transport.stop();
      Serial.println("seq: stop (export)");
    }
    exporter.start(pattern, currentBpm, kDefaultOneShots, mixer, Serial, SERIAL_BAUD);
    renderUiMode();
  } else if (cmd == NavCommand::Back || cmd == NavCommand::Cancel) {
    exportEmptyShown = false;
    setMode(UiMode::MenuList);
  }
}

void navMenuItem(NavCommand cmd) {
  switch (menuOpenItem) {
    case kMenuItemExport:
      navExport(cmd);
      return;
    case kMenuItemTempo:
      // PAD6/PAD2 — строка, PAD1/PAD3 — меньше/больше, PAD5 — метроном.
      if (cmd == NavCommand::Up || cmd == NavCommand::Down) {
        tempoRow = tempoRow == 0 ? 1 : 0;
        ui.showTempo(currentBpm, metronomeOn, tempoRow);
      } else if (cmd == NavCommand::Back || cmd == NavCommand::Forward) {
        if (tempoRow == 0) {
          setBpm((int32_t)currentBpm + (cmd == NavCommand::Forward ? 1 : -1));
        } else {
          setMetronomeEnabled(!metronomeOn);
        }
      } else if (cmd == NavCommand::Confirm) {
        setMetronomeEnabled(!metronomeOn);
      } else if (cmd == NavCommand::Cancel) {
        setMode(UiMode::MenuList);
      }
      return;
    case kMenuItemInput:
      if (cmd == NavCommand::Back && knobSpeedIndex > 0) {
        knobSpeedIndex--;
        ui.showInput(KNOB_SPEEDS[knobSpeedIndex], lastKnob, lastKnobDelta);
      } else if (cmd == NavCommand::Forward && knobSpeedIndex + 1 < KNOB_SPEED_COUNT) {
        knobSpeedIndex++;
        ui.showInput(KNOB_SPEEDS[knobSpeedIndex], lastKnob, lastKnobDelta);
      } else if (cmd == NavCommand::Cancel) {
        setMode(UiMode::MenuList);
      }
      return;
    default:
      if (cmd == NavCommand::Back || cmd == NavCommand::Cancel) setMode(UiMode::MenuList);
      return;
  }
}

void handleNavCommand(NavCommand cmd) {
  if (cmd == NavCommand::None || !onHome()) return;

  // PAD4 — метроном с любого экрана (B.4).
  if (cmd == NavCommand::ContextB) {
    setMetronomeEnabled(!metronomeOn);
    return;
  }

  switch (uiMode) {
    case UiMode::HomeMain:
      navHome(cmd);
      break;
    case UiMode::Sequencer:
      navSequencer(cmd);
      break;
    case UiMode::Mixer:
      navMixer(cmd);
      break;
    case UiMode::PianoRoll:
      navPianoRoll(cmd);
      break;
    case UiMode::Arrangement:
      navArrangement(cmd);
      break;
    case UiMode::MenuList:
      navMenuList(cmd);
      break;
    case UiMode::MenuItem:
      navMenuItem(cmd);
      break;
  }
}

// ---------------------------------------------------------------------------
// Фоновые обновления: экспорт, бегущий шаг, индикаторы.

// Экспорт идёт порциями внутри loop(), чтобы экран обновлял прогресс, а
// PAD7 мог его отменить.
void updateExport() {
  if (!exportRunning()) return;

  const uint8_t before = exporter.percent();
  exporter.process();
  const bool onExportPage =
      onHome() && uiMode == UiMode::MenuItem && menuOpenItem == kMenuItemExport;
  if (!exportRunning()) {
    Serial.printf("export: done, %s\n", exporter.fileName());
    if (onExportPage) {
      renderUiMode();
    } else {
      exportUnseen = exporter.state() == PatternExporter::State::Done;
      refreshHome();
    }
  } else if (exporter.percent() != before) {
    if (onExportPage) {
      ui.updateExportProgress(exporter.percent());
    } else {
      refreshHome();
    }
  }
}

// Шаг наступает в задаче звука на ядре 0 (transport.h), там же запускаются
// голоса. Здесь только разбираются уже случившиеся события: строка в лог и
// бегущий шаг на экране. Если loop() занят перерисовкой, события подождут в
// очереди — на момент удара это больше не влияет.
void drainSequencerEvents() {
  static const char* const kTrackLogNames[StepSequencer::kTracks] = {"KICK", "SNARE", "HAT",
                                                                     "PERC"};
  uint8_t step = 0;
  uint8_t mask = 0;
  while (transport.takeFired(step, mask)) {
    if (mask == 0) continue;  // шаг пустой — в лог писать нечего
    Serial.printf("seq: step %u:", step + 1);
    for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
      if (mask & (uint8_t)(1u << t)) Serial.printf(" %s", kTrackLogNames[t]);
    }
    Serial.println();
  }

  const uint8_t ph = transport.playhead();
  if (ph == lastPlayhead) return;
  if (ph != Transport::kPlayheadIdle && lastPlayhead != Transport::kPlayheadIdle &&
      ph < lastPlayhead) {
    loopCount++;
  }
  lastPlayhead = ph;
  if (!onHome()) return;

  switch (uiMode) {
    case UiMode::Sequencer:
      ui.updateSequencerPlayhead(transport.pattern(), ph, seqCursorTrack, seqCursorStep);
      break;
    case UiMode::PianoRoll:
      ui.updatePianoPlayhead(pianoRoll, ph, rollStep, rollPitch);
      break;
    case UiMode::Arrangement:
      ui.updateArrangementPlayhead(arrangementPlayBar());
      break;
    case UiMode::HomeMain:
      refreshHome();  // номер такта в шапке
      break;
    default:
      break;
  }
}

// Индикатор OUT на главном экране и индикаторы полос микшера показывают
// реальный пиковый уровень (то, что движок отдал на выход с прошлого
// кадра), а не факт "что-то играет".
void updateMeters() {
  const unsigned long now = millis();
  if (now - lastMeterDrawAt < METER_REDRAW_INTERVAL_MS) return;
  lastMeterDrawAt = now;

  auto ballistics = [](float& level, float peak) {
    level *= 0.85f;  // спад — баллистика индикатора
    if (peak > level) level = peak;
    if (level < 0.02f) level = 0.0f;
  };

  const float master = audio.takePeak() / 32767.0f;
  ballistics(soundMeterLevel, master);
  for (uint8_t i = 0; i < AudioEngine::kVoices; i++) {
    ballistics(mixerMeterLevels[i], audio.takeVoicePeak(i) / 32767.0f);
  }
  mixerMeterLevels[MixerSettings::kMasterStrip] = soundMeterLevel;

  if (!onHome()) return;
  if (uiMode == UiMode::HomeMain) {
    ui.updateSoundMeter(soundMeterLevel);
  } else if (uiMode == UiMode::Mixer) {
    ui.updateMixerMeters(mixerMeterLevels);
  }
}

void updateClearConfirm(unsigned long now) {
  if (clearConfirmTrack != 255 && now - clearConfirmAt >= CLEAR_CONFIRM_MS) cancelClearConfirm();
}

// ---------------------------------------------------------------------------

void logEvent(const InputEvent& ev) {
  switch (ev.type) {
    case InputEventType::NoteOn:
      Serial.printf("NoteOn  ch=%d note=%d vel=%d\n", ev.channel, ev.number, ev.value);
      break;
    case InputEventType::NoteOff:
      Serial.printf("NoteOff ch=%d note=%d\n", ev.channel, ev.number);
      break;
    case InputEventType::ControlChange:
      Serial.printf("CC      ch=%d cc=%d val=%d\n", ev.channel, ev.number, ev.value);
      break;
    case InputEventType::KnobTurn:
      Serial.printf("Knob    K%d %+d\n", ev.number - MPK_KNOB_CC_BASE + 1, ev.delta);
      break;
  }
}

void handleInputEvent(const InputEvent& ev) {
  handlePowerButton(ev);
  handleModeButton(ev);
  handlePlayStopButton(ev);
  handleKnob(ev);
  handleChannelKey(ev);
  if (ev.type == InputEventType::NoteOn) {
    handleNavCommand(navCommandForNoteOn(ev.channel, ev.number));
  }
  logEvent(ev);
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);  // дать монитору порта время открыться в Wokwi
  Serial.println("ESP32-S3 sampler-sequencer: старт");
  logBoardMemory(Serial);
  ui.begin();
  audio.begin(kAudioSampleRate);
  audio.applyMixer(mixer);
  transport.begin(kAudioSampleRate, audio, kDefaultOneShots);
  transport.setBpm(currentBpm);
  transport.setMetronome(metronomeOn);
  audioOutput.begin(transport, AUDIO_PIN);
  enterOff();
  // Опрос ввода стартует последним: к первому событию всё уже готово.
  inputSource.begin();
}

void loop() {
  unsigned long now = millis();
  if (now - lastBeat >= BEAT_INTERVAL_MS) {
    lastBeat = now;
    // audio=N — сколько сэмплов вывод выдал за секунду; должно быть ~16000,
    // иначе таймер звука работает не на той частоте.
    // edges=N — сколько раз за секунду переключался звуковой пин.
    // under=N — сколько раз прерывание пришло к пустому кольцевому буферу.
    // В норме 0; ненулевое значение означает, что задача звука на ядре 0 не
    // успевает досыпать буфер и в звуке дырки.
    static uint32_t lastSamplesOut = 0;
    static uint32_t lastEdges = 0;
    static uint32_t lastUnderruns = 0;
    const uint32_t samplesOut = audioOutput.samplesOut();
    const uint32_t edges = audioOutput.edges();
    const uint32_t underruns = audioOutput.underruns();
    Serial.printf("heartbeat audio=%u edges=%u under=%u\n",
                  (unsigned)(samplesOut - lastSamplesOut), (unsigned)(edges - lastEdges),
                  (unsigned)(underruns - lastUnderruns));
    lastSamplesOut = samplesOut;
    lastEdges = edges;
    lastUnderruns = underruns;
  }

  // Ввод копится в очереди задачей опроса (wokwi_input_source.h) даже
  // пока loop() занят перерисовкой, поэтому здесь разбираем всё, что
  // накопилось.
  InputEvent ev;
  while (inputSource.poll(ev)) handleInputEvent(ev);

  now = millis();
  updatePowerButton(now);
  if (powerState == PowerState::Boot) updateBoot();
  updateClearConfirm(now);

  drainSequencerEvents();
  updateExport();
  updateMeters();
  // Отдать процессор простою до следующего тика: часы секвенсора считают
  // шаги в задаче звука, так что задержка здесь ритм не сдвигает. Во время
  // экспорта не ждём — он и так упирается в скорость порта.
  if (!exportRunning()) delay(1);
}
