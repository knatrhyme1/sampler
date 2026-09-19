"""Регрессионный тест звукового движка: блочный рендер против посэмплового.

Движок перешёл с `AudioEngine::renderSample()` на `renderBlock(out, n)`
(docs/known-issues.md, закрытый п. 5). Звук при этом обязан остаться
прежним бит в бит, иначе меняется и живой вывод, и экспортируемый WAV.
Этот скрипт держит оба алгоритма рядом и сверяет их выход на наборе
сценариев, включая вырожденные размеры блока.

Целочисленная математика движка не переполняет int32, поэтому точные int
Python воспроизводят её один в один:

    |next - s| <= 65535, frac>>1 <= 32767 -> 65535*32767 = 2 147 385 345 < 2^31

Арифметический сдвиг вправо у отрицательных чисел в C++ (gcc) и в Python
совпадает — оба округляют вниз.

Сэмплы берутся из firmware/default_samples.h, то есть тест идёт по тем же
данным, что и прошивка.

Запуск из корня репозитория:

    python tools/audio_bitexact_test.py

Код возврата 0 — все сценарии совпали, 1 — есть расхождение.
"""

import re
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve()
FW = None
for base in (ROOT.parent.parent, Path.cwd()):
    cand = base / "firmware" / "default_samples.h"
    if cand.exists():
        FW = cand
        break
if FW is None and len(sys.argv) > 1:
    FW = Path(sys.argv[1])
if FW is None:
    raise SystemExit(
        "не найден firmware/default_samples.h — запускать из корня репозитория "
        "или передать путь к файлу аргументом"
    )

SRC = FW.read_text(encoding="utf-8")

CHANNEL_GAIN = 160
METRONOME_GAIN = 256
MASTER_GAIN = 256
VOICES = 5
CHANNEL_VOICES = 4
MAX_BLOCK = 64
# Сетка секвенсора (firmware/step_sequencer.h): 16 шагов четвертями = 4 такта,
# экспорт проигрывает паттерн один раз (PatternExporter::kLoops).
STEPS = 16
STEPS_PER_BEAT = 1
LOOPS = 1


def parse_sample(name):
    m = re.search(
        r"static const int16_t %s\[\] PROGMEM = \{(.*?)\};" % name, SRC, re.S
    )
    if not m:
        raise SystemExit("не найден массив %s" % name)
    vals = [int(x) for x in m.group(1).replace("\n", "").split(",") if x.strip()]
    n = int(re.search(r"static const uint32_t %sLength = (\d+);" % name, SRC).group(1))
    assert len(vals) == n, "%s: %d значений против длины %d" % (name, len(vals), n)
    return vals


KIT = [parse_sample(n) for n in ("kSampleKick", "kSampleSnare", "kSampleHat", "kSamplePerc")]
SAMPLES_RATE = int(re.search(r"kDefaultSamplesRate = (\d+);", SRC).group(1))


def s16(v):
    return v if v < 32768 else v - 65536


class Voice:
    __slots__ = ("data", "length", "pos", "frac", "step", "gain", "active")

    def __init__(self):
        self.data = None
        self.length = 0
        self.pos = 0
        self.frac = 0
        self.step = 0
        self.gain = 0
        self.active = False


def start(v, data, length, step, gain):
    v.data, v.length, v.pos, v.frac, v.step, v.gain = data, length, 0, 0, step, gain
    v.active = length > 0


def master(mix):
    mix = (mix * MASTER_GAIN) >> 8
    if mix > 32767:
        mix = 32767
    if mix < -32768:
        mix = -32768
    return mix


# --- старый путь: один сэмпл за вызов, как renderSample() ---------------
def render_sample_old(voices):
    mix = 0
    for v in voices:
        if not v.active:
            continue
        s = v.data[v.pos]
        if v.frac != 0:
            nxt = v.data[v.pos + 1] if v.pos + 1 < v.length else 0
            s += ((nxt - s) * (v.frac >> 1)) >> 15
        mix += (s * v.gain) >> 8
        v.frac += v.step
        v.pos += v.frac >> 16
        v.frac &= 0xFFFF
        if v.pos >= v.length:
            v.active = False
    return master(mix)


# --- новый путь: блок за вызов, как renderBlock() -----------------------
def render_block_new(voices, n):
    acc = [0] * n
    for v in voices:
        if not v.active:
            continue
        data, length, step, gain = v.data, v.length, v.step, v.gain
        pos, frac = v.pos, v.frac
        for k in range(n):
            s = data[pos]
            nxt = data[pos + 1] if pos + 1 < length else 0
            s += ((nxt - s) * (frac >> 1)) >> 15
            acc[k] += (s * gain) >> 8
            frac += step
            pos += frac >> 16
            frac &= 0xFFFF
            if pos >= length:
                v.active = False
                break
        v.pos, v.frac = pos, frac
    return [master(a) for a in acc]


def step_q16(sample_rate, output_rate):
    return (sample_rate << 16) // output_rate


def click_table(output_rate, freq):
    import math

    n = output_rate * 30 // 1000
    out = []
    for i in range(n):
        t = i / output_rate
        env = math.exp(-t * 180.0)
        # float32 в прошивке против float64 здесь: щелчок метронома в тесте
        # не участвует, таблица нужна только для полноты сценария.
        out.append(int(math.sin(2.0 * math.pi * freq * t) * env * 0.35 * 32767))
    return out


def run(output_rate, bpm, pattern, total_samples, block_limit, kit=None):
    """Гоняет оба пути по одному сценарию и возвращает две дорожки."""
    kit = kit if kit is not None else KIT
    steps = STEPS
    total_steps = steps * LOOPS

    def step_start(i):
        return (i * output_rate * 60) // (bpm * STEPS_PER_BEAT)

    st = [step_q16(SAMPLES_RATE, output_rate) for _ in kit]

    # --- посэмплово ---
    va = [Voice() for _ in range(VOICES)]
    out_a = []
    nxt = 0
    for idx in range(total_samples):
        while nxt < total_steps and idx >= step_start(nxt):
            s = nxt % steps
            for t in range(CHANNEL_VOICES):
                if pattern[t] >> s & 1:
                    start(va[t], kit[t], len(kit[t]), st[t], CHANNEL_GAIN)
            nxt += 1
        out_a.append(render_sample_old(va))

    # --- блоками, с обрывом на границе шага ---
    vb = [Voice() for _ in range(VOICES)]
    out_b = []
    nxt = 0
    idx = 0
    while idx < total_samples:
        while nxt < total_steps and idx >= step_start(nxt):
            s = nxt % steps
            for t in range(CHANNEL_VOICES):
                if pattern[t] >> s & 1:
                    start(vb[t], kit[t], len(kit[t]), st[t], CHANNEL_GAIN)
            nxt += 1
        limit = total_samples
        if nxt < total_steps:
            limit = min(limit, step_start(nxt))
        n = min(limit - idx, block_limit)
        if n <= 0:
            break
        out_b.extend(render_block_new(vb, n))
        idx += n

    return out_a, out_b


def abrupt_kit():
    """Ваншоты, которые кончаются не тишиной, а полной амплитудой.

    У настоящих ударных последний сэмпл — ноль, поэтому ошибка на границе
    голоса (обрыв на сэмпл раньше или позже) на них не видна: и там и там
    в микс уходит ноль. Здесь хвост ненулевой, и такая ошибка сразу даёт
    расхождение.
    """
    return [
        [20000] * 200,                       # прямоугольник, обрыв на +20000
        [-32768] * 97,                        # нечётная длина, обрыв на минимуме
        [(-1) ** i * 15000 for i in range(53)],  # чередование до самого конца
        [32767] * 3,                          # короче блока
    ]


def crc(vals):
    b = bytearray()
    for v in vals:
        u = v & 0xFFFF
        b.append(u & 0xFF)
        b.append(u >> 8)
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def main():
    scenarios = [
        # (имя, частота вывода, bpm, паттерн по каналам, сколько сэмплов,
        #  блок[, набор ваншотов вместо штатного])
        ("экспорт 44,1 кГц, плотный паттерн", 44100, 120,
         [0b1000100010001000, 0b0000100000001000, 0b1010101010101010, 0b0010000000100000],
         44100 * 3, 64),
        ("живой вывод 16 кГц, блок 32", 16000, 120,
         [0b1000100010001000, 0b0000100000001000, 0b1111111111111111, 0b0000000000000001],
         16000 * 3, 32),
        ("быстрый темп 240, все каналы на каждом шаге", 44100, 240,
         [0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF], 44100 * 2, 64),
        ("медленный темп 40, редкие удары", 16000, 40,
         [0b0000000000000001, 0, 0, 0b1000000000000000], 16000 * 4, 32),
        ("пустой паттерн (тишина)", 16000, 120, [0, 0, 0, 0], 16000, 32),
        ("блок 1 сэмпл — вырожденный случай", 16000, 120,
         [0b1010101010101010, 0, 0b1111000011110000, 0], 16000, 1),
        ("блок 7 сэмплов — не делитель шага", 44100, 137,
         [0b1000100010001000, 0b0001000100010001, 0, 0b0100000001000000], 44100 * 2, 7),
        ("обрыв ваншота на полной амплитуде, блок 32", 16000, 120,
         [0b1000100010001000, 0b0010001000100010, 0b1111111111111111, 0b1000000000000000],
         16000 * 2, 32, abrupt_kit()),
        ("то же, блок 1 сэмпл", 16000, 200,
         [0b1010101010101010, 0b0101010101010101, 0b1111000011110000, 0b0000000000000001],
         16000, 1, abrupt_kit()),
        ("то же на 44,1 кГц — шаг по ваншоту 2,76", 44100, 90,
         [0b1000100010001000, 0, 0b1111111111111111, 0b0001000100010001],
         44100, 16, abrupt_kit()),
    ]

    ok = True
    print("%-46s %10s %10s  %s" % ("сценарий", "старый", "новый", "результат"))
    print("-" * 82)
    for scenario in scenarios:
        name, rate, bpm, pattern, total, block = scenario[:6]
        kit = scenario[6] if len(scenario) > 6 else None
        a, b = run(rate, bpm, pattern, total, block, kit)
        ca, cb = crc(a), crc(b)
        same = a == b and ca == cb
        if not same:
            ok = False
            diff = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), None)
            extra = "  первое расхождение: сэмпл %s (%s против %s)" % (
                diff, a[diff] if diff is not None else "-", b[diff] if diff is not None else "-")
        else:
            extra = ""
        print("%-46s %08x %08x  %s%s" % (name, ca, cb, "совпало" if same else "РАСХОЖДЕНИЕ", extra))
        if len(a) != len(b):
            ok = False
            print("    длины не совпали: %d против %d" % (len(a), len(b)))

    print()
    print("ИТОГ:", "все сценарии бит в бит" if ok else "ЕСТЬ РАСХОЖДЕНИЯ")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
