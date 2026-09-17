// Экспорт паттерна в WAV (44,1 кГц, 16 бит, моно). Паттерн рендерится тем
// же звуковым движком и мастер-шиной, что и живой звук, но офлайн — не в
// реальном времени, поэтому результат не зависит от скорости симуляции и не
// "заикается".
//
// Файл уходит в Serial текстом, построчно, чтобы его можно было забрать из
// Serial Monitor в Wokwi (скачать файл из симулятора иначе нельзя) — это
// делает закладка из tools/export_bookmarklet.html:
//
//   EXPORT-BEGIN id=<hex> file=<name>.wav bytes=<N> lines=<M>
//   E:<номер строки>:<base64, 57 байт файла на строку>
//   ...
//   EXPORT-END id=<hex> crc32=<hex>          (или EXPORT-ABORT id=<hex>)
//
// На время передачи порт переключается на 2 000 000 бод: Wokwi честно
// соблюдает скорость порта, и на 115 200 бод 8-секундный файл шёл бы минуты.
#pragma once

#include <Arduino.h>

#include "audio_engine.h"
#include "step_sequencer.h"

class PatternExporter {
 public:
  static constexpr uint32_t kSampleRate = 44100;
  static const uint8_t kLoops = 2;  // сколько раз паттерн повторяется в файле

  enum class State : uint8_t { Idle, Running, Done, Aborted };

  struct Plan {
    uint32_t totalSamples;
    uint32_t fileBytes;
    uint32_t durationMs;
  };

  // Сколько получится файл для такого паттерна — без запуска экспорта. В
  // конец добавляется хвост длиной в самый длинный ваншот из звучащих
  // каналов, чтобы последние удары не обрывались.
  static Plan plan(const StepSequencer& pattern, uint16_t bpm, const OneShot* kit);

  // serial — порт, в который пишется файл; normalBaud — скорость, на
  // которую он возвращается после экспорта.
  void start(const StepSequencer& pattern, uint16_t bpm, const OneShot* kit,
             HardwareSerial& serial, uint32_t normalBaud);
  // Рендерит и отправляет следующую порцию; вызывать из loop(), пока
  // state() == Running.
  void process();
  void abort();
  // Возвращает экспортёр в Idle (если экспорт не идёт).
  void reset();

  State state() const { return state_; }
  uint8_t percent() const;
  const char* fileName() const { return fileName_; }
  const Plan& currentPlan() const { return plan_; }

 private:
  static constexpr uint32_t kExportBaud = 2000000;
  static const uint8_t kBytesPerLine = 57;  // -> 76 символов base64
  static const uint16_t kSamplesPerChunk = 1024;

  uint32_t stepStartSample(uint32_t step) const;
  void pushByte(uint8_t b);
  void pushSample(int16_t s);
  void pushTag(const char* tag);
  void pushU16(uint16_t v);
  void pushU32(uint32_t v);
  void emitLine();
  void finish(bool completed);

  AudioEngine engine_;
  StepSequencer pattern_;
  const OneShot* kit_ = nullptr;
  HardwareSerial* serial_ = nullptr;
  uint32_t normalBaud_ = 115200;
  uint16_t bpm_ = 120;

  State state_ = State::Idle;
  Plan plan_ = {};
  uint32_t id_ = 0;
  char fileName_[32] = "";

  uint32_t sampleIndex_ = 0;
  uint32_t nextStep_ = 0;
  uint32_t totalSteps_ = 0;
  uint32_t crc_ = 0;
  uint32_t bytesOut_ = 0;
  uint32_t lineIndex_ = 0;
  uint8_t lineBuf_[kBytesPerLine] = {};
  uint8_t lineLen_ = 0;
};
