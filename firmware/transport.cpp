#include "transport.h"

void Transport::begin(uint32_t sampleRate, AudioEngine& engine, const OneShot* kit) {
  engine_ = &engine;
  kit_ = kit;
  sampleRate_ = sampleRate;
  setTempo(bpm_);
  stepSamples_ = nextStepSamples();
  playhead_ = kPlayheadIdle;
}

// Шаг — восьмая нота: 60 / bpm / kStepsPerBeat секунды.
void Transport::setTempo(uint16_t bpm) {
  if (bpm == 0) bpm = 1;
  tempoBpm_ = bpm;
  stepDiv_ = (uint32_t)bpm * StepSequencer::kStepsPerBeat;
  const uint32_t total = sampleRate_ * 60;
  stepBase_ = total / stepDiv_;
  stepRem_ = total % stepDiv_;
  remAcc_ = 0;
}

uint32_t Transport::nextStepSamples() {
  uint32_t n = stepBase_;
  remAcc_ += stepRem_;
  if (remAcc_ >= stepDiv_) {
    remAcc_ -= stepDiv_;
    n++;
  }
  return n;
}

void Transport::play() {
  portENTER_CRITICAL(&mux_);
  playing_ = true;
  restart_ = true;
  portEXIT_CRITICAL(&mux_);
}

void Transport::stop() {
  portENTER_CRITICAL(&mux_);
  playing_ = false;
  portEXIT_CRITICAL(&mux_);
}

bool Transport::playing() const {
  portENTER_CRITICAL(&mux_);
  const bool p = playing_;
  portEXIT_CRITICAL(&mux_);
  return p;
}

void Transport::setBpm(uint16_t bpm) {
  portENTER_CRITICAL(&mux_);
  bpm_ = bpm;
  portEXIT_CRITICAL(&mux_);
}

void Transport::setMetronome(bool on) {
  portENTER_CRITICAL(&mux_);
  metronome_ = on;
  portEXIT_CRITICAL(&mux_);
}

void Transport::toggleStep(uint8_t track, uint8_t step) {
  if (track >= StepSequencer::kTracks || step >= StepSequencer::kSteps) return;
  portENTER_CRITICAL(&mux_);
  pattern_.toggle(track, step);
  portEXIT_CRITICAL(&mux_);
}

StepSequencer Transport::pattern() const {
  portENTER_CRITICAL(&mux_);
  const StepSequencer copy = pattern_;
  portEXIT_CRITICAL(&mux_);
  return copy;
}

uint8_t Transport::playhead() const { return playhead_; }

void Transport::pushFired(uint8_t step, uint8_t mask) {
  const uint8_t next = (uint8_t)((firedHead_ + 1) % kFiredQueue);
  if (next == firedTail_) return;  // UI не успевает разбирать — теряем лог, не звук
  firedStep_[firedHead_] = step;
  firedMask_[firedHead_] = mask;
  __sync_synchronize();  // запись видна UI раньше, чем сдвинется голова
  firedHead_ = next;
}

bool Transport::takeFired(uint8_t& step, uint8_t& mask) {
  const uint8_t tail = firedTail_;
  if (tail == firedHead_) return false;
  step = firedStep_[tail];
  mask = firedMask_[tail];
  firedTail_ = (uint8_t)((tail + 1) % kFiredQueue);
  return true;
}

void Transport::fireStep(const StepSequencer& pattern, bool metronome) {
  // Метроном щёлкает по тем же часам, что и паттерн, поэтому доли всегда
  // совпадают с ударами.
  if (metronome && step_ % StepSequencer::kStepsPerBeat == 0) {
    engine_->triggerMetronome(step_ % StepSequencer::kStepsPerBar == 0);
  }

  uint8_t mask = 0;
  for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
    if (!pattern.isOn(t, step_)) continue;
    engine_->trigger(t, kit_[t]);
    mask |= (uint8_t)(1u << t);
  }

  playhead_ = step_;
  pushFired(step_, mask);
}

void Transport::render(int16_t* out, uint16_t n) {
  // Снимок всего, что мог поменять UI, — один раз на блок. Дальше рендер
  // идёт без замка: держать его весь блок значило бы запретить прерывания
  // на всю его длительность.
  bool playing, restart, metronome;
  uint16_t bpm;
  StepSequencer pattern;
  portENTER_CRITICAL(&mux_);
  playing = playing_;
  restart = restart_;
  restart_ = false;
  bpm = bpm_;
  metronome = metronome_;
  pattern = pattern_;
  portEXIT_CRITICAL(&mux_);

  if (!playing) {
    playhead_ = kPlayheadIdle;
    pendingFire_ = false;
    engine_->renderBlock(out, n);
    return;
  }

  if (restart) {
    step_ = 0;
    intoStep_ = 0;
    setTempo(bpm);
    stepSamples_ = nextStepSamples();
    pendingFire_ = true;  // первый шаг звучит сразу, не через интервал
  } else if (bpm != tempoBpm_) {
    // Новый темп берётся со следующего шага: текущий доигрывает свою длину.
    setTempo(bpm);
  }

  uint16_t done = 0;
  while (done < n) {
    // Шаг ставится перед рендером своего куска, поэтому голос, который он
    // запустил, начинается ровно с первого сэмпла этого куска.
    if (pendingFire_) {
      pendingFire_ = false;
      fireStep(pattern, metronome);
    }

    const uint32_t left = (stepSamples_ > intoStep_) ? (stepSamples_ - intoStep_) : 1;
    const uint32_t room = (uint32_t)(n - done);
    const uint16_t chunk = (uint16_t)(left < room ? left : room);

    engine_->renderBlock(out + done, chunk);
    done += chunk;
    intoStep_ += chunk;

    if (intoStep_ >= stepSamples_) {
      intoStep_ -= stepSamples_;
      step_ = (uint8_t)((step_ + 1) % StepSequencer::kSteps);
      stepSamples_ = nextStepSamples();  // темп читается на каждом шаге
      pendingFire_ = true;
    }
  }
}
