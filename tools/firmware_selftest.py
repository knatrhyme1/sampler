"""Самотест прошивки: настоящий C++ звукового движка и транспорта против эталона.

Регрессии на Python (audio_bitexact_test.py, transport_timing_test.py)
проверяют модель алгоритма, а не код прошивки: сломанный audio_engine.cpp
они не заметят. Этот скрипт закрывает разрыв. Он собирает скетч из
исходников прошивки как есть и вшивает в него эталонные CRC, посчитанные
Python-моделью. Скетч запускается на ESP32-S3 (в Wokwi или на плате) и сам
печатает, совпал ли звук.

Что проверяет скетч:

- engine    — AudioEngine::renderBlock, блоки режутся на границах шагов,
              как в экспорте (PatternExporter::process);
- transport — Transport::render, то есть живой путь: те же сценарии, куски
              того размера, который просит вывод, в том числе 256 — как
              просил бы I2S-вывод;
- timing    — по одному щелчку на каждый шаг: шаг k обязан звучать ровно на
              сэмпле floor(k * rate * 60 / bpm), как в экспорте.

Запуск из корня репозитория:

    python tools/firmware_selftest.py            # собрать скетч
    python tools/firmware_selftest.py --compile  # и проверить сборку arduino-cli

Скетч появится в build/firmware_selftest/firmware_selftest.ino. Дальше:
открыть https://wokwi.com/projects/new/esp32-s3, заменить sketch.ino его
содержимым и запустить. Последняя строка в Serial Monitor —
"SELFTEST: PASS" или "SELFTEST: FAIL".
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware"
OUT_DIR = ROOT / "build" / "firmware_selftest"
OUT_FILE = OUT_DIR / "firmware_selftest.ino"

sys.path.insert(0, str(ROOT / "tools"))
import audio_bitexact_test as ref  # noqa: E402  (эталонная модель движка)

# Исходники прошивки, которые попадают в скетч без изменений — в порядке
# зависимостей.
SOURCES = [
    "audio_source.h",
    "step_sequencer.h",
    "default_samples.h",
    "audio_engine.h",
    "audio_engine.cpp",
    "transport.h",
    "transport.cpp",
]

# Размер куска, который просил бы I2S-вывод (ESP-IDF: dma_frame_num = 240 по
# умолчанию, округлено до степени двойки). Больше AudioEngine::kMaxBlockSamples.
I2S_LIKE_BLOCK = 256

TIMING_BPMS = (40, 90, 120, 137, 150, 200, 240)
TIMING_STEPS = 40  # 2,5 круга паттерна; перенос остатка при 137 BPM — каждые ~3 шага


def firmware_sources():
    parts = []
    for name in SOURCES:
        src = (FIRMWARE / name).read_text(encoding="utf-8").replace("\r\n", "\n")
        src = re.sub(r"^#pragma once\s*$", "", src, flags=re.M)
        src = re.sub(r'^#include "[^"]+"\s*$', "", src, flags=re.M)
        parts.append("// ===== firmware/%s =====\n%s" % (name, src))
    return "\n".join(parts)


def c_array(name, values):
    body = ",".join(str(v) for v in values)
    return "static const int16_t %s[] = {%s};" % (name, body)


def c_string(text):
    return '"%s"' % text.replace("\\", "\\\\").replace('"', '\\"')


def scenario_table():
    """Строки таблицы сценариев и эталонные CRC из Python-модели."""
    rows = []
    for scenario in ref.SCENARIOS:
        name, rate, bpm, pattern, total, block = scenario[:6]
        kit = scenario[6] if len(scenario) > 6 else None
        per_sample, blocks = ref.run(rate, bpm, pattern, total, block, kit)
        if per_sample != blocks:
            raise SystemExit("эталон расходится сам с собой: %s" % name)
        rows.append("  {%s, %u, %u, {%s}, %u, %u, %s, 0x%08xUL}," % (
            c_string(name), rate, bpm, ", ".join("0x%04x" % p for p in pattern),
            total, block, "kAbruptKit" if kit is not None else "kDefaultKit",
            ref.crc(blocks)))
    return "\n".join(rows)


def generate():
    abrupt = ref.abrupt_kit()
    abrupt_arrays = "\n".join(c_array("kAbrupt%d" % i, v) for i, v in enumerate(abrupt))
    abrupt_kit = ", ".join(
        "{kAbrupt%d, %u, kDefaultSamplesRate}" % (i, len(v)) for i, v in enumerate(abrupt))

    return TEMPLATE % {
        "sources": firmware_sources(),
        "abrupt_arrays": abrupt_arrays,
        "abrupt_kit": abrupt_kit,
        "scenarios": scenario_table(),
        "i2s_block": I2S_LIKE_BLOCK,
        "timing_bpms": ", ".join(str(b) for b in TIMING_BPMS),
        "timing_steps": TIMING_STEPS,
    }


TEMPLATE = r"""// Сгенерировано tools/firmware_selftest.py — не править руками.
// Ниже исходники прошивки без изменений (убраны только локальные #include и
// #pragma once), затем сам тест.
#include <Arduino.h>

%(sources)s

// ===== самотест =====
%(abrupt_arrays)s

static const OneShot kDefaultKit[StepSequencer::kTracks] = {
    {kSampleKick, kSampleKickLength, kDefaultSamplesRate},
    {kSampleSnare, kSampleSnareLength, kDefaultSamplesRate},
    {kSampleHat, kSampleHatLength, kDefaultSamplesRate},
    {kSamplePerc, kSamplePercLength, kDefaultSamplesRate},
};
static const OneShot kAbruptKit[StepSequencer::kTracks] = {%(abrupt_kit)s};

struct Scenario {
  const char* name;
  uint32_t rate;
  uint16_t bpm;
  uint16_t pattern[StepSequencer::kTracks];  // бит N — шаг N
  uint32_t totalSamples;
  uint16_t block;
  const OneShot* kit;
  uint32_t crc;  // эталон Python-модели (audio_bitexact_test.py)
};

static const Scenario kScenarios[] = {
%(scenarios)s
};

static const uint16_t kI2sLikeBlock = %(i2s_block)d;
static const uint16_t kTimingBpms[] = {%(timing_bpms)s};
static const uint32_t kTimingSteps = %(timing_steps)d;

// CRC-32 (как zlib.crc32) по сэмплам int16 в порядке little-endian.
static uint32_t gCrcTable[256];
static void crcInit() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
    gCrcTable[i] = c;
  }
}
static uint32_t crcAdd(uint32_t crc, const int16_t* s, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    const uint16_t u = (uint16_t)s[i];
    crc = gCrcTable[(crc ^ (u & 0xFF)) & 0xFF] ^ (crc >> 8);
    crc = gCrcTable[(crc ^ (u >> 8)) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

static uint32_t stepStart(uint32_t k, uint32_t rate, uint16_t bpm) {
  return (uint32_t)((uint64_t)k * rate * 60 / ((uint32_t)bpm * StepSequencer::kStepsPerBeat));
}

static uint32_t gPassed = 0;
static uint32_t gFailed = 0;

static void report(bool ok, const char* group, const char* name, uint32_t got, uint32_t want) {
  if (ok) {
    gPassed++;
    Serial.printf("PASS %%-9s %%s\n", group, name);
  } else {
    gFailed++;
    Serial.printf("FAIL %%-9s %%s: got=%%08x want=%%08x\n", group, name, (unsigned)got,
                  (unsigned)want);
  }
}

static AudioEngine gEngine;
static Transport gTransport;
static int16_t gBuf[kI2sLikeBlock];

// Экспортный путь: блоки режутся на границах шагов, заявки ставятся перед
// блоком — так же, как в PatternExporter::process.
static uint32_t runEngine(const Scenario& sc) {
  gEngine.begin(sc.rate);
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t idx = 0;
  uint32_t next = 0;
  while (idx < sc.totalSamples) {
    while (next < StepSequencer::kSteps && idx >= stepStart(next, sc.rate, sc.bpm)) {
      for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
        if ((sc.pattern[t] >> next) & 1) gEngine.trigger(t, sc.kit[t]);
      }
      next++;
    }
    uint32_t limit = sc.totalSamples;
    if (next < StepSequencer::kSteps) limit = min(limit, stepStart(next, sc.rate, sc.bpm));
    uint32_t n = min(limit - idx, (uint32_t)sc.block);
    n = min(n, (uint32_t)AudioEngine::kMaxBlockSamples);
    gEngine.renderBlock(gBuf, (uint16_t)n);
    crc = crcAdd(crc, gBuf, n);
    idx += n;
  }
  return crc ^ 0xFFFFFFFFUL;
}

static void startTransport(uint32_t rate, uint16_t bpm, const uint16_t* pattern,
                           const OneShot* kit) {
  gEngine.begin(rate);
  gTransport = Transport();
  gTransport.begin(rate, gEngine, kit);
  gTransport.setBpm(bpm);
  for (uint8_t t = 0; t < StepSequencer::kTracks; t++) {
    for (uint8_t s = 0; s < StepSequencer::kSteps; s++) {
      if ((pattern[t] >> s) & 1) gTransport.toggleStep(t, s);
    }
  }
  gTransport.play();
}

// Живой путь: вывод просит у транспорта куски по block сэмплов.
static uint32_t runTransport(const Scenario& sc, uint16_t block) {
  startTransport(sc.rate, sc.bpm, sc.pattern, sc.kit);
  uint32_t crc = 0xFFFFFFFFUL;
  for (uint32_t done = 0; done < sc.totalSamples;) {
    const uint16_t n = (uint16_t)min((uint32_t)block, sc.totalSamples - done);
    gTransport.render(gBuf, n);
    crc = crcAdd(crc, gBuf, n);
    done += n;
  }
  return crc ^ 0xFFFFFFFFUL;
}

// Щелчок длиной в один сэмпл на каждом шаге: ненулевой сэмпл на выходе —
// ровно момент, когда транспорт запустил шаг.
static const int16_t kImpulse[] = {20000};

static void runTiming(uint32_t rate, uint16_t bpm, const uint16_t* blocks, uint8_t blockCount,
                      const char* label) {
  OneShot kit[StepSequencer::kTracks] = {
      {kImpulse, 1, rate}, {kImpulse, 1, rate}, {kImpulse, 1, rate}, {kImpulse, 1, rate}};
  const uint16_t pattern[StepSequencer::kTracks] = {0xFFFF, 0, 0, 0};
  startTransport(rate, bpm, pattern, kit);

  const uint32_t total = stepStart(kTimingSteps, rate, bpm);
  uint32_t k = 0;
  uint32_t badStep = 0xFFFFFFFFUL;
  uint32_t badAt = 0;
  uint32_t pos = 0;
  for (uint8_t bi = 0; pos < total; bi++) {
    const uint16_t n = (uint16_t)min((uint32_t)blocks[bi %% blockCount], total - pos);
    gTransport.render(gBuf, n);
    for (uint16_t i = 0; i < n; i++, pos++) {
      if (gBuf[i] == 0) continue;
      if (badStep == 0xFFFFFFFFUL && pos != stepStart(k, rate, bpm)) {
        badStep = k;
        badAt = pos;
      }
      k++;
    }
  }
  char name[64];
  snprintf(name, sizeof(name), "%%u Hz %%u bpm, %%s, %%u steps", (unsigned)rate, bpm, label,
           (unsigned)kTimingSteps);
  const bool ok = badStep == 0xFFFFFFFFUL && k == kTimingSteps;
  report(ok, "timing", name, ok ? k : badAt,
         badStep == 0xFFFFFFFFUL ? kTimingSteps : stepStart(badStep, rate, bpm));
}

void setup() {
  Serial.begin(115200);
  delay(300);
  crcInit();
  Serial.println("SELFTEST: start");

  for (const Scenario& sc : kScenarios) {
    const uint32_t got = runEngine(sc);
    report(got == sc.crc, "engine", sc.name, got, sc.crc);
  }

  for (const Scenario& sc : kScenarios) {
    char name[160];
    snprintf(name, sizeof(name), "%%s / render %%u", sc.name, sc.block);
    const uint32_t got = runTransport(sc, sc.block);
    report(got == sc.crc, "transport", name, got, sc.crc);

    snprintf(name, sizeof(name), "%%s / render %%u (I2S-like)", sc.name, kI2sLikeBlock);
    const uint32_t gotBig = runTransport(sc, kI2sLikeBlock);
    report(gotBig == sc.crc, "transport", name, gotBig, sc.crc);
  }

  const uint16_t live[] = {32};
  const uint16_t mixed[] = {7, 13, 32};
  const uint16_t exportBlock[] = {64};
  for (uint16_t bpm : kTimingBpms) {
    runTiming(16000, bpm, live, 1, "render 32");
    runTiming(16000, bpm, mixed, 3, "render 7/13/32");
  }
  runTiming(44100, 137, exportBlock, 1, "render 64");

  Serial.printf("SELFTEST: %%s (%%u passed, %%u failed)\n", gFailed == 0 ? "PASS" : "FAIL",
                (unsigned)gPassed, (unsigned)gFailed);
}

void loop() { delay(1000); }
"""


def find_arduino_cli():
    found = shutil.which("arduino-cli")
    if found:
        return found
    home = Path.home() / ".arduino15" / "cli-bin" / "arduino-cli.exe"
    return str(home) if home.exists() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--compile", action="store_true",
                        help="проверить, что скетч собирается под ESP32-S3 (arduino-cli)")
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    OUT_FILE.write_text(generate(), encoding="utf-8", newline="\n")
    print("скетч:", OUT_FILE.relative_to(ROOT))

    if args.compile:
        cli = find_arduino_cli()
        if cli is None:
            raise SystemExit("arduino-cli не найден")
        result = subprocess.run([cli, "compile", "--fqbn", "esp32:esp32:esp32s3", str(OUT_DIR)])
        return result.returncode
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
