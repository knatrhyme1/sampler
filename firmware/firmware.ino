// Стори 0.2: пустой проект на ESP32-S3, живой в Wokwi.
// Раз в секунду выводит сообщение в лог — доказательство, что плата
// запускается и код работает.
//
// Плюс черновая проверка виртуального контроллера в Wokwi (стори 3.1/3.2):
// WokwiInputSource реализует общий интерфейс InputSource, поэтому его можно
// будет заменить на чтение настоящего MPK mini по USB-MIDI (стори 3.3), не
// трогая остальной код.
//
// Плюс экран включения/загрузки/приветствия (см. docs/boot-screen-brief.md):
// кнопка POWER (CC DEVICE_CC_POWER) переключает устройство между "выключено"
// и загрузкой, после которой показывается главный экран.

#include "wokwi_input_source.h"
#include "ui_screens.h"
#include "control_layout.h"
#include "step_sequencer.h"
#include "audio_engine.h"
#include "audio_output_1bit.h"
#include "default_samples.h"
#include "pattern_export.h"

const uint32_t SERIAL_BAUD = 115200;
const unsigned long BEAT_INTERVAL_MS = 1000;
const unsigned long POWER_HOLD_MS = 600;  // защита от случайного нажатия
const unsigned long BOOT_DURATION_MS = 3000;
const unsigned long BOOT_FRAME_MS = 60;  // ~16 fps перерисовки глитч-анимации
const uint16_t DEFAULT_BPM = 120;
const uint16_t BPM_MIN = 40;
const uint16_t BPM_MAX = 240;
const uint8_t DEFAULT_SECTION = 0;

// Звуковой выход мастер-шины: 1-битный поток на GPIO21, к нему в
// diagram.json подключена пищалка bz1 (см. audio_output_1bit.h).
const uint8_t AUDIO_PIN = 21;
const unsigned long METER_REDRAW_INTERVAL_MS = 30;
const unsigned long INPUT_POLL_MS = 2;

enum class PowerState : uint8_t { Off, Boot, Home };

// Экраны внутри PowerState::Home (B.2): главный экран, страница раздела
// проекта (раздел 1 — степ-секвенсор, остальные пока заглушки) или меню
// (список пунктов / открытый пункт). SHIFT (удержание MODE) пока не
// реализован — короткое нажатие MODE всегда переключает Home <-> меню.
enum class UiMode : uint8_t { HomeMain, SectionPage, Sequencer, MenuList, MenuItem };

const uint8_t SEQUENCER_SECTION = 0;

WokwiInputSource inputSource;
UiScreens ui;
StepSequencer sequencer;
uint8_t seqCursorTrack = 0;
uint8_t seqCursorStep = 0;
AudioEngine audio;
OneBitAudioOutput audioOutput;
PatternExporter exporter;

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
unsigned long lastInputPollAt = 0;
PowerState powerState = PowerState::Off;
UiMode uiMode = UiMode::HomeMain;
uint8_t activeSection = DEFAULT_SECTION;
// Курсор главного экрана только показывает, на чём стоит селектор; выбор
// (нажатие) делает PAD5. activeSection меняется только нажатием.
HomeFocus homeFocus = HomeFocus::Sections;
uint8_t sectionCursor = DEFAULT_SECTION;
uint16_t currentBpm = DEFAULT_BPM;
uint8_t menuSelected = 0;
uint8_t menuOpenItem = 0;

// Настройка "метроном участвует в проигрывании". Сама по себе ничего не
// запускает: щелчки идут только пока транспорт играет (PLAY), на каждой
// четверти по часам секвенсора.
bool metronomeOn = false;
float soundMeterLevel = 0.0f;
unsigned long lastMeterDrawAt = 0;

bool powerButtonDown = false;
unsigned long powerButtonDownAt = 0;
bool powerActionTriggered = false;

unsigned long bootStartedAt = 0;
unsigned long bootFrameShownAt = 0;

void enterOff() {
  exporter.abort();
  sequencer.stop();
  audio.stopAll();
  soundMeterLevel = 0.0f;
  powerState = PowerState::Off;
  ui.showOff();
  Serial.println("power: off");
}

void enterBoot() {
  powerState = PowerState::Boot;
  bootStartedAt = millis();
  bootFrameShownAt = 0;
  Serial.println("power: boot");
}

ExportStatus exportStatus() {
  switch (exporter.state()) {
    case PatternExporter::State::Running:
      return ExportStatus::Running;
    case PatternExporter::State::Done:
      return ExportStatus::Done;
    case PatternExporter::State::Aborted:
      return ExportStatus::Aborted;
    default:
      return ExportStatus::Ready;
  }
}

// Пока экспорт не запускали, страница показывает размер будущего файла для
// текущего паттерна; после запуска — размер того, что ушло/уходит.
void renderExportPage() {
  const PatternExporter::Plan plan = exporter.state() == PatternExporter::State::Idle
                                         ? PatternExporter::plan(sequencer, currentBpm,
                                                                 kDefaultOneShots)
                                         : exporter.currentPlan();
  ui.showExport(exportStatus(), exporter.percent(), currentBpm, plan.durationMs, plan.fileBytes,
                exporter.fileName());
}

void renderUiMode() {
  switch (uiMode) {
    case UiMode::HomeMain:
      ui.showHome(currentBpm, activeSection, sectionCursor, homeFocus, metronomeOn,
                  sequencer.playing());
      break;
    case UiMode::SectionPage:
      ui.showSectionPage(activeSection);
      break;
    case UiMode::Sequencer:
      ui.showSequencer(sequencer, seqCursorTrack, seqCursorStep, currentBpm);
      break;
    case UiMode::MenuList:
      ui.showMenuList(menuSelected);
      break;
    case UiMode::MenuItem:
      if (menuOpenItem == kMenuItemExport) {
        renderExportPage();
      } else {
        ui.showMenuItem(menuOpenItem);
      }
      break;
  }
}

void enterHome() {
  powerState = PowerState::Home;
  uiMode = UiMode::HomeMain;
  renderUiMode();
  Serial.println("power: home/ready");
}

// MODE (короткое нажатие) переключает Home <-> список меню. Удержание
// (SHIFT) — открытый вопрос B.4, пока не реализовано.
void handleModeButton(const InputEvent& ev) {
  if (ev.type != InputEventType::ControlChange || ev.number != DEVICE_CC_MODE) return;
  if (ev.value == 0) return;  // реагируем на нажатие, не на отпускание
  if (powerState != PowerState::Home) return;

  if (uiMode == UiMode::HomeMain) {
    uiMode = UiMode::MenuList;
    menuSelected = 0;
  } else {
    uiMode = UiMode::HomeMain;
  }
  renderUiMode();
}

// Кнопка MET только решает, будет ли метроном щёлкать вместе с остальным при
// проигрывании. Если транспорт уже играет, щелчки начнутся/прекратятся со
// следующей доли.
void setMetronomeEnabled(bool on) { metronomeOn = on; }

// Нижние клавиши C3..D#3 (B.4) проигрывают ваншот канала 1–4 через ту же
// мастер-шину — прослушать звук, не запуская паттерн. На странице
// секвенсора клавиша ещё и переводит курсор на этот канал.
void handleChannelKey(const InputEvent& ev) {
  if (ev.type != InputEventType::NoteOn || ev.channel != MPK_CHANNEL_KEYS) return;
  if (ev.number < MPK_KEY_LOWEST || ev.number >= MPK_KEY_LOWEST + StepSequencer::kTracks) return;
  if (powerState != PowerState::Home) return;

  const uint8_t track = ev.number - MPK_KEY_LOWEST;
  audio.trigger(track, kDefaultOneShots[track]);
  if (uiMode == UiMode::Sequencer && track != seqCursorTrack) {
    const uint8_t oldTrack = seqCursorTrack;
    seqCursorTrack = track;
    ui.updateSequencerCursor(sequencer, oldTrack, seqCursorStep, seqCursorTrack, seqCursorStep);
  }
}

// Страница EXPORT: PAD5 — старт (транспорт при этом останавливается: экспорт
// рендерит паттерн сам, офлайн), PAD7 — отмена идущего экспорта или выход,
// PAD1 — выход, если экспорт не идёт.
void handleExportNav(NavCommand cmd) {
  const bool running = exporter.state() == PatternExporter::State::Running;
  if (running) {
    if (cmd == NavCommand::Cancel) {
      exporter.abort();
      Serial.println("export: cancelled");
      renderUiMode();
    }
    return;
  }

  if (cmd == NavCommand::Confirm) {
    if (sequencer.playing()) {
      sequencer.stop();
      Serial.println("seq: stop (export)");
    }
    exporter.start(sequencer, currentBpm, kDefaultOneShots, Serial, SERIAL_BAUD);
    renderUiMode();
  } else if (cmd == NavCommand::Back || cmd == NavCommand::Cancel) {
    uiMode = UiMode::MenuList;
    renderUiMode();
  }
}

// Экспорт идёт порциями внутри loop(), чтобы экран обновлял прогресс, а
// PAD7 мог его отменить.
void updateExport() {
  if (exporter.state() != PatternExporter::State::Running) return;

  const uint8_t before = exporter.percent();
  exporter.process();
  const bool onExportPage = powerState == PowerState::Home && uiMode == UiMode::MenuItem &&
                            menuOpenItem == kMenuItemExport;
  if (exporter.state() != PatternExporter::State::Running) {
    Serial.printf("export: done, %s\n", exporter.fileName());
    if (onExportPage) renderUiMode();
  } else if (onExportPage && exporter.percent() != before) {
    ui.updateExportProgress(exporter.percent());
  }
}

// Навигационный кластер (B.2/B.4, docs/poc-backlog-draft.md): один и тот же
// набор команд ведёт себя по-разному в зависимости от текущего экрана.
void handleNavCommand(NavCommand cmd) {
  if (cmd == NavCommand::None || powerState != PowerState::Home) return;

  switch (uiMode) {
    case UiMode::HomeMain:
      // PAD6/PAD2 — курсор между верхней строкой (BPM, MET) и рядом разделов
      // проекта 1–4; PAD1/PAD3 — влево/вправо внутри строки (B.4). PAD5
      // нажимает элемент под курсором: MET — вкл/выкл метроном; раздел —
      // первое нажатие выбирает его, нажатие на уже выбранный открывает его
      // страницу. У BPM нажатия нет — темп крутится K1, пока курсор на нём.
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
        sectionCursor = (sectionCursor == 0) ? 3 : sectionCursor - 1;
      } else if (cmd == NavCommand::Forward && homeFocus == HomeFocus::Sections) {
        sectionCursor = (sectionCursor + 1) % 4;
      } else if (cmd == NavCommand::Confirm && homeFocus == HomeFocus::Metronome) {
        setMetronomeEnabled(!metronomeOn);
      } else if (cmd == NavCommand::Confirm && homeFocus == HomeFocus::Sections) {
        if (sectionCursor != activeSection) {
          activeSection = sectionCursor;
        } else {
          uiMode = (activeSection == SEQUENCER_SECTION) ? UiMode::Sequencer : UiMode::SectionPage;
        }
      } else {
        break;
      }
      renderUiMode();
      break;

    case UiMode::SectionPage:
      if (cmd == NavCommand::Cancel) {
        uiMode = UiMode::HomeMain;
        renderUiMode();
      }
      break;

    case UiMode::Sequencer: {
      // Сетка как таблица: PAD6/PAD2 — канал вверх/вниз, PAD1/PAD3 — шаг
      // влево/вправо (с переходом через край, чтобы с 16-го шага попасть на
      // 1-й одним нажатием). PAD7 ставит/убирает удар под курсором — поэтому
      // здесь PAD7 не "назад"; выход на главный экран — PAD5 (или MODE).
      const uint8_t oldTrack = seqCursorTrack;
      const uint8_t oldStep = seqCursorStep;
      if (cmd == NavCommand::Up) {
        seqCursorTrack = (seqCursorTrack == 0) ? StepSequencer::kTracks - 1 : seqCursorTrack - 1;
      } else if (cmd == NavCommand::Down) {
        seqCursorTrack = (seqCursorTrack + 1) % StepSequencer::kTracks;
      } else if (cmd == NavCommand::Back) {
        seqCursorStep = (seqCursorStep == 0) ? StepSequencer::kSteps - 1 : seqCursorStep - 1;
      } else if (cmd == NavCommand::Forward) {
        seqCursorStep = (seqCursorStep + 1) % StepSequencer::kSteps;
      } else if (cmd == NavCommand::Confirm) {
        // PAD5 — выход на главный экран. Проигрывание не прерывается.
        uiMode = UiMode::HomeMain;
        renderUiMode();
        break;
      } else if (cmd == NavCommand::Cancel) {
        sequencer.toggle(seqCursorTrack, seqCursorStep);
        ui.updateSequencerCell(sequencer, seqCursorTrack, seqCursorStep, seqCursorTrack,
                               seqCursorStep);
        break;
      } else {
        break;
      }
      ui.updateSequencerCursor(sequencer, oldTrack, oldStep, seqCursorTrack, seqCursorStep);
      break;
    }

    case UiMode::MenuList:
      if (cmd == NavCommand::Up) {
        menuSelected = (menuSelected == 0) ? kMenuItemCount - 1 : menuSelected - 1;
        renderUiMode();
      } else if (cmd == NavCommand::Down) {
        menuSelected = (menuSelected + 1) % kMenuItemCount;
        renderUiMode();
      } else if (cmd == NavCommand::Forward || cmd == NavCommand::Confirm) {
        uiMode = UiMode::MenuItem;
        menuOpenItem = menuSelected;
        if (menuOpenItem == kMenuItemExport && exporter.state() != PatternExporter::State::Running) {
          exporter.reset();  // страница открывается в состоянии READY
        }
        renderUiMode();
      } else if (cmd == NavCommand::Back || cmd == NavCommand::Cancel) {
        uiMode = UiMode::HomeMain;
        renderUiMode();
      }
      break;

    case UiMode::MenuItem:
      if (menuOpenItem == kMenuItemExport) {
        handleExportNav(cmd);
      } else if (cmd == NavCommand::Back || cmd == NavCommand::Cancel) {
        uiMode = UiMode::MenuList;
        renderUiMode();
      }
      break;
  }
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

// K1 (CC MPK_KNOB_CC_BASE, первый потенциометр pot1 в diagram.json) крутит
// BPM — но только пока курсор главного экрана стоит на значении BPM. В любой
// другой момент это же событие CC70 игнорируется, как договаривались.
void handleBpmKnob(const InputEvent& ev) {
  if (ev.type != InputEventType::ControlChange) return;
  if (ev.channel != MPK_CHANNEL_KNOBS || ev.number != MPK_KNOB_CC_BASE) return;
  if (powerState != PowerState::Home || uiMode != UiMode::HomeMain) return;
  if (homeFocus != HomeFocus::Bpm) return;

  const uint16_t bpm = BPM_MIN + (uint16_t)(((uint32_t)ev.value * (BPM_MAX - BPM_MIN)) / 127);
  if (bpm != currentBpm) {
    currentBpm = bpm;
    renderUiMode();
  }
}

// Индикатор OUT на главном экране показывает реальный пиковый уровень
// мастер-шины (то, что движок отдал на выход с прошлого кадра), а не
// факт "что-то играет".
void updateOutputMeter() {
  const unsigned long now = millis();
  if (now - lastMeterDrawAt < METER_REDRAW_INTERVAL_MS) return;
  lastMeterDrawAt = now;

  const float peak = audio.takePeak() / 32767.0f;
  soundMeterLevel *= 0.85f;  // спад — баллистика индикатора
  if (peak > soundMeterLevel) soundMeterLevel = peak;
  if (soundMeterLevel < 0.02f) soundMeterLevel = 0.0f;

  if (powerState == PowerState::Home && uiMode == UiMode::HomeMain) {
    ui.updateSoundMeter(soundMeterLevel);
  }
}

// PLAY/STOP (стори 1.2): единый транспорт проекта. Старт всегда с первого
// шага; переходы между экранами проигрывание не прерывают — остановить его
// можно только этой кнопкой (или выключением питания). Анимация бегущего
// шага видна на странице секвенсора, на главном экране — статус PLAY.
void handlePlayStopButton(const InputEvent& ev) {
  if (ev.type != InputEventType::ControlChange || ev.number != DEVICE_CC_PLAY_STOP) return;
  if (ev.value == 0) return;
  if (powerState != PowerState::Home) return;

  if (sequencer.playing()) {
    sequencer.stop();  // уже звучащие удары доигрывают свой хвост
    Serial.println("seq: stop");
  } else {
    sequencer.start(micros());
    Serial.println("seq: play");
  }
  if (uiMode == UiMode::Sequencer) {
    ui.updateSequencerTransport(sequencer.playing(), currentBpm);
    ui.updateSequencerPlayhead(sequencer, seqCursorTrack, seqCursorStep);
  } else if (uiMode == UiMode::HomeMain) {
    ui.updateHomeTransport(sequencer.playing());
  }
}

// Часы секвенсора — они же часы метронома, поэтому щелчки всегда ровно на
// долях паттерна. Сработавшие на шаге каналы запускают свои ваншоты в
// движке (мастер-шина), дублируются в лог и подсвечиваются на экране.
void updateSequencer() {
  if (!sequencer.update(micros(), currentBpm)) return;

  const uint8_t step = sequencer.currentStep();
  if (metronomeOn && step % StepSequencer::kStepsPerBeat == 0) {
    audio.triggerMetronome(step % StepSequencer::kStepsPerBar == 0);
  }
  static const char* const kTrackLogNames[StepSequencer::kTracks] = {"KICK", "SNARE", "HAT",
                                                                     "PERC"};
  bool any = false;
  for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
    if (!sequencer.isOn(t, step)) continue;
    audio.trigger(t, kDefaultOneShots[t]);
    if (!any) Serial.printf("seq: step %u:", step + 1);
    Serial.printf(" %s", kTrackLogNames[t]);
    any = true;
  }
  if (any) Serial.println();

  if (powerState == PowerState::Home && uiMode == UiMode::Sequencer) {
    ui.updateSequencerPlayhead(sequencer, seqCursorTrack, seqCursorStep);
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
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);  // дать монитору порта время открыться в Wokwi
  Serial.println("ESP32-S3 sampler-sequencer: старт");
  inputSource.begin();
  ui.begin();
  audio.begin(kAudioSampleRate);
  audioOutput.begin(audio, AUDIO_PIN);
  enterOff();
}

void loop() {
  unsigned long now = millis();
  if (now - lastBeat >= BEAT_INTERVAL_MS) {
    lastBeat = now;
    // audio=N — сколько сэмплов вывод выдал за секунду; должно быть ~16000,
    // иначе таймер звука работает не на той частоте.
    // edges=N — сколько раз за секунду переключался звуковой пин.
    static uint32_t lastSamplesOut = 0;
    static uint32_t lastEdges = 0;
    const uint32_t samplesOut = audioOutput.samplesOut();
    const uint32_t edges = audioOutput.edges();
    Serial.printf("heartbeat audio=%u edges=%u\n", (unsigned)(samplesOut - lastSamplesOut),
                  (unsigned)(edges - lastEdges));
    lastSamplesOut = samplesOut;
    lastEdges = edges;
  }

  // Контроллер опрашивается раз в INPUT_POLL_MS, а не на каждом витке: опрос
  // сдвиговых регистров — это busy-wait на delayMicroseconds, и без паузы он
  // держал процессор загруженным на 100%, что тормозило симуляцию Wokwi (а с
  // ней и звук).
  InputEvent ev;
  bool haveEvent = false;
  if (now - lastInputPollAt >= INPUT_POLL_MS) {
    lastInputPollAt = now;
    haveEvent = inputSource.poll(ev);
  }
  if (haveEvent) {
    handlePowerButton(ev);
    handleModeButton(ev);
    handlePlayStopButton(ev);
    handleBpmKnob(ev);
    handleChannelKey(ev);
    if (ev.type == InputEventType::NoteOn) {
      handleNavCommand(navCommandForNoteOn(ev.channel, ev.number));
    }
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
    }
  }

  // Долгое удержание POWER (>= POWER_HOLD_MS) переключает питание. Во время
  // загрузки кнопка игнорируется — процесс нельзя прервать.
  if (powerButtonDown && !powerActionTriggered && (now - powerButtonDownAt) >= POWER_HOLD_MS) {
    powerActionTriggered = true;
    if (powerState == PowerState::Off) {
      enterBoot();
    } else if (powerState == PowerState::Home) {
      enterOff();
    }
  }

  if (powerState == PowerState::Boot) {
    updateBoot();
  }

  updateSequencer();
  updateExport();
  updateOutputMeter();
  // Отдать процессор простою до следующего тика: часы секвенсора считают
  // шаги по micros() от расписания, так что 1 мс задержки не накапливается.
  // Во время экспорта не ждём — он и так упирается в скорость порта.
  if (exporter.state() != PatternExporter::State::Running) delay(1);
}
