#include "pattern_export.h"

namespace {
constexpr uint32_t kWavHeaderBytes = 44;
constexpr const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint32_t crc32Table[256];
bool crc32TableReady = false;

void buildCrc32Table() {
  if (crc32TableReady) return;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (uint8_t k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
    crc32Table[i] = c;
  }
  crc32TableReady = true;
}

bool trackHasSteps(const StepSequencer& pattern, uint8_t track) {
  for (uint8_t s = 0; s < StepSequencer::kSteps; s++) {
    if (pattern.isOn(track, s)) return true;
  }
  return false;
}
}  // namespace

PatternExporter::Plan PatternExporter::plan(const StepSequencer& pattern, uint16_t bpm,
                                            const OneShot* kit) {
  const uint32_t patternSteps = (uint32_t)StepSequencer::kSteps * kLoops;
  // Шаг — доля: 60 / (bpm * kStepsPerBeat) секунды.
  const uint32_t patternSamples = (uint32_t)((uint64_t)patternSteps * kSampleRate * 60 /
                                             ((uint32_t)bpm * StepSequencer::kStepsPerBeat));

  uint32_t tail = 0;
  for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
    if (!trackHasSteps(pattern, t)) continue;
    const uint32_t len =
        (uint32_t)(((uint64_t)kit[t].length * kSampleRate + kit[t].sampleRate - 1) /
                   kit[t].sampleRate);
    tail = max(tail, len);
  }

  Plan p;
  p.totalSamples = patternSamples + tail;
  p.fileBytes = kWavHeaderBytes + p.totalSamples * 2;
  p.durationMs = (uint32_t)((uint64_t)p.totalSamples * 1000 / kSampleRate);
  return p;
}

uint32_t PatternExporter::stepStartSample(uint32_t step) const {
  return (uint32_t)((uint64_t)step * kSampleRate * 60 /
                    ((uint32_t)bpm_ * StepSequencer::kStepsPerBeat));
}

void PatternExporter::start(const StepSequencer& pattern, uint16_t bpm, const OneShot* kit,
                            const MixerSettings& mix, HardwareSerial& serial,
                            uint32_t normalBaud) {
  if (state_ == State::Running) return;
  buildCrc32Table();

  pattern_ = pattern;
  bpm_ = bpm;
  kit_ = kit;
  serial_ = &serial;
  normalBaud_ = normalBaud;
  plan_ = plan(pattern, bpm, kit);

  engine_.begin(kSampleRate);
  engine_.applyMixer(mix);
  sampleIndex_ = 0;
  nextStep_ = 0;
  totalSteps_ = (uint32_t)StepSequencer::kSteps * kLoops;
  crc_ = 0xFFFFFFFFUL;
  bytesOut_ = 0;
  lineIndex_ = 0;
  lineLen_ = 0;
  id_ = esp_random();
  snprintf(fileName_, sizeof(fileName_), "smplr-pattern-%ubpm.wav", (unsigned)bpm);
  state_ = State::Running;

  serial_->flush();
  serial_->updateBaudRate(kExportBaud);
  serial_->printf("\nEXPORT-BEGIN id=%08x file=%s bytes=%u lines=%u bpl=%u\n", (unsigned)id_,
                  fileName_, (unsigned)plan_.fileBytes,
                  (unsigned)((plan_.fileBytes + kBytesPerLine - 1) / kBytesPerLine),
                  (unsigned)kBytesPerLine);

  // Заголовок WAV (PCM, моно, 16 бит).
  const uint32_t dataBytes = plan_.totalSamples * 2;
  pushTag("RIFF");
  pushU32(36 + dataBytes);
  pushTag("WAVE");
  pushTag("fmt ");
  pushU32(16);
  pushU16(1);  // PCM
  pushU16(1);  // моно
  pushU32(kSampleRate);
  pushU32(kSampleRate * 2);  // байт в секунду
  pushU16(2);                // байт на кадр
  pushU16(16);               // бит на сэмпл
  pushTag("data");
  pushU32(dataBytes);
}

void PatternExporter::process() {
  if (state_ != State::Running) return;

  int16_t block[kExportBlockSamples];
  uint32_t produced = 0;

  while (produced < kSamplesPerChunk && sampleIndex_ < plan_.totalSamples) {
    while (nextStep_ < totalSteps_ && sampleIndex_ >= stepStartSample(nextStep_)) {
      const uint8_t step = nextStep_ % StepSequencer::kSteps;
      for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
        if (pattern_.isOn(t, step)) engine_.trigger(t, kit_[t]);
      }
      nextStep_++;
    }

    // Блок обрывается на начале следующего шага: движок применяет заявки
    // на запуск голосов в начале блока, поэтому удар должен попадать на
    // первый сэмпл блока, а не на середину. Так офлайн-рендер остаётся
    // сэмпл в сэмпл тем же, что и при посэмпловом рендере.
    uint32_t limit = plan_.totalSamples;
    if (nextStep_ < totalSteps_) limit = min(limit, stepStartSample(nextStep_));
    uint32_t n = min(limit - sampleIndex_, (uint32_t)kExportBlockSamples);
    n = min(n, (uint32_t)(kSamplesPerChunk - produced));
    if (n == 0) break;

    engine_.renderBlock(block, (uint16_t)n);
    for (uint32_t i = 0; i < n; i++) pushSample(block[i]);
    sampleIndex_ += n;
    produced += n;
  }

  if (sampleIndex_ >= plan_.totalSamples) finish(true);
}

void PatternExporter::abort() {
  if (state_ == State::Running) finish(false);
}

uint8_t PatternExporter::percent() const {
  if (plan_.fileBytes == 0) return 0;
  return (uint8_t)((uint64_t)bytesOut_ * 100 / plan_.fileBytes);
}

void PatternExporter::pushByte(uint8_t b) {
  crc_ = crc32Table[(crc_ ^ b) & 0xFF] ^ (crc_ >> 8);
  bytesOut_++;
  lineBuf_[lineLen_++] = b;
  if (lineLen_ == kBytesPerLine) emitLine();
}

void PatternExporter::pushSample(int16_t s) {
  pushByte((uint8_t)(s & 0xFF));
  pushByte((uint8_t)((uint16_t)s >> 8));
}

void PatternExporter::pushTag(const char* tag) {
  for (uint8_t i = 0; i < 4; i++) pushByte((uint8_t)tag[i]);
}

void PatternExporter::pushU16(uint16_t v) {
  pushByte((uint8_t)v);
  pushByte((uint8_t)(v >> 8));
}

void PatternExporter::pushU32(uint32_t v) {
  pushU16((uint16_t)v);
  pushU16((uint16_t)(v >> 16));
}

void PatternExporter::emitLine() {
  if (lineLen_ == 0) return;
  // "E:" + номер + ":" + base64 строки + "\n"
  char out[kLineTextMax];
  int n = snprintf(out, sizeof(out), "E:%u:", (unsigned)lineIndex_);
  for (uint16_t i = 0; i < lineLen_; i += 3) {
    const uint32_t chunk = ((uint32_t)lineBuf_[i] << 16) |
                           (i + 1 < lineLen_ ? (uint32_t)lineBuf_[i + 1] << 8 : 0) |
                           (i + 2 < lineLen_ ? (uint32_t)lineBuf_[i + 2] : 0);
    out[n++] = kBase64[(chunk >> 18) & 0x3F];
    out[n++] = kBase64[(chunk >> 12) & 0x3F];
    out[n++] = (i + 1 < lineLen_) ? kBase64[(chunk >> 6) & 0x3F] : '=';
    out[n++] = (i + 2 < lineLen_) ? kBase64[chunk & 0x3F] : '=';
  }
  out[n++] = '\n';
  serial_->write((const uint8_t*)out, n);
  lineIndex_++;
  lineLen_ = 0;
}

void PatternExporter::finish(bool completed) {
  if (completed) {
    emitLine();  // хвост последней неполной строки
    serial_->printf("EXPORT-END id=%08x crc32=%08x\n", (unsigned)id_,
                    (unsigned)(crc_ ^ 0xFFFFFFFFUL));
    state_ = State::Done;
  } else {
    serial_->printf("\nEXPORT-ABORT id=%08x\n", (unsigned)id_);
    state_ = State::Aborted;
  }
  serial_->flush();
  serial_->updateBaudRate(normalBaud_);
  serial_->println();
  engine_.stopAll();
}
