// Экспорт паттерна в WAV (44,1 кГц, 16 бит, моно). Паттерн рендерится тем
// же звуковым движком и мастер-шиной, что и живой звук, но офлайн — не в
// реальном времени, поэтому результат не зависит от скорости симуляции и не
// "заикается".
//
// Файл уходит в Serial текстом, построчно, чтобы его можно было забрать из
// Serial Monitor в Wokwi (скачать файл из симулятора иначе нельзя) — это
// делает закладка из tools/export_bookmarklet.html:
//
//   EXPORT-BEGIN id=<hex> file=<name>.wav bytes=<N> lines=<M> bpl=<байт в строке>
//   E:<номер строки>:<base64 этих байт>
//   ...
//   EXPORT-END id=<hex> crc32=<hex>          (или EXPORT-ABORT id=<hex>)
//
// bpl в заголовке нужен, чтобы приёмник не знал длину строки заранее: её
// можно менять здесь, не трогая закладку.
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
  // Сколько раз паттерн повторяется в файле. Паттерн — 4 такта, этого
  // хватает, чтобы послушать его целиком.
  static const uint8_t kLoops = 1;

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
  // которую он возвращается после экспорта. mix — настройки микшера: файл
  // звучит с теми же громкостями, что и устройство (метроном в файл не
  // попадает).
  void start(const StepSequencer& pattern, uint16_t bpm, const OneShot* kit,
             const MixerSettings& mix, HardwareSerial& serial, uint32_t normalBaud);
  // Рендерит и отправляет следующую порцию; вызывать из loop(), пока
  // state() == Running.
  void process();
  void abort();

  State state() const { return state_; }
  uint8_t percent() const;
  const char* fileName() const { return fileName_; }
  const Plan& currentPlan() const { return plan_; }
  // Темп, с которым идёт (или прошёл) последний экспорт.
  uint16_t bpm() const { return bpm_; }

 private:
  static constexpr uint32_t kExportBaud = 2000000;
  // Байт файла на строку. Кратно трём, чтобы base64 не добивался знаками
  // "=" в середине файла. Чем длиннее строка, тем меньше строк набирается
  // в Serial Monitor браузера — а именно их число, а не объём, роняет
  // скорость приёма к концу большого экспорта.
  static const uint16_t kBytesPerLine = 171;  // -> 228 символов base64
  // "E:" + номер + ":" + base64 + перевод строки, с запасом.
  static const uint16_t kLineTextMax = 4 * ((kBytesPerLine + 2) / 3) + 24;
  static const uint16_t kSamplesPerChunk = 1024;
  // Порция, которой офлайн-рендер дёргает движок. Блок дополнительно
  // обрывается на границе шага — см. process().
  static const uint16_t kExportBlockSamples = AudioEngine::kMaxBlockSamples;

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
  uint16_t lineLen_ = 0;
};
