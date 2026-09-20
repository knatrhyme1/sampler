"""Регрессионный тест часов транспорта: попадает ли шаг ровно на свой сэмпл.

Музыкальное время считается в сэмплах внутри задачи звука (firmware/transport.cpp,
`Transport::render`). Граница шага k обязана вставать в

    floor(k * sampleRate * 60 / (bpm * kStepsPerBeat))

— то есть ровно туда же, куда её ставит офлайн-рендер экспорта
(`PatternExporter::stepStartSample`), иначе живой звук и экспортированный WAV
разъедутся по ритму.

Наивное `длина_шага = rate * 60 / bpm` с округлением вниз этого НЕ даёт:
на 137 BPM шаг равен 7007,30 сэмпла, и накопленное округление уводит сетку
на ~2,6 мс за каждую минуту (16 кГц). Поэтому в коде остаток от деления копится
(`nextStepSamples`), и этот скрипт проверяет, что получается точно.

Здесь же проверяется независимость от размера блока: вывод дёргает транспорт
кусками по 32 сэмпла, экспорт — по 64, а границы шагов на них не попадают.

Запуск из корня репозитория:

    python tools/transport_timing_test.py

Код возврата 0 — всё сходится, 1 — есть расхождение.
"""

import sys

STEPS = 16
STEPS_PER_BEAT = 1


def schedule(rate, bpm, block_sizes, total_samples):
    """Повторяет логику Transport::render и возвращает [(сэмпл, номер шага)]."""
    div = bpm * STEPS_PER_BEAT
    total = rate * 60
    base, rem = divmod(total, div)

    state = {"acc": 0}

    def next_step_samples():
        n = base
        state["acc"] += rem
        if state["acc"] >= div:
            state["acc"] -= div
            n += 1
        return n

    step = 0
    into = 0
    step_samples = next_step_samples()
    pending = True          # первый шаг звучит сразу, не через интервал
    fired = []
    pos = 0
    produced = 0
    bi = 0

    while produced < total_samples:
        n = min(block_sizes[bi % len(block_sizes)], total_samples - produced)
        bi += 1
        done = 0
        while done < n:
            if pending:
                pending = False
                fired.append((pos, step))
            left = step_samples - into if step_samples > into else 1
            chunk = min(left, n - done)
            done += chunk
            pos += chunk
            into += chunk
            if into >= step_samples:
                into -= step_samples
                step = (step + 1) % STEPS
                step_samples = next_step_samples()
                pending = True
        produced += n
    return fired


def expected(rate, bpm, k):
    """Эталон — та же формула, что в PatternExporter::stepStartSample."""
    return k * rate * 60 // (bpm * STEPS_PER_BEAT)


def check(rate, bpm, block_sizes, seconds):
    total = rate * seconds
    fired = schedule(rate, bpm, block_sizes, total)
    worst = 0
    worst_k = 0
    for k, (pos, step) in enumerate(fired):
        want = expected(rate, bpm, k)
        if step != k % STEPS:
            return None, "шаг %d пришёл с номером %d" % (k, step)
        d = abs(pos - want)
        if d > worst:
            worst, worst_k = d, k
    return (worst, worst_k, len(fired)), None


def main():
    cases = []
    for bpm in (40, 90, 120, 137, 150, 200, 240):
        for name, blocks in (
            ("блок 32 (живой вывод)", [32]),
            ("блок 64 (экспорт)", [64]),
            ("блоки 7/13/32 вперемешку", [7, 13, 32]),
            ("блок 1", [1]),
        ):
            cases.append((16000, bpm, name, blocks))
    # частота экспорта — та же проверка против той же формулы
    for bpm in (90, 137, 240):
        cases.append((44100, bpm, "44,1 кГц, блок 64", [64]))

    ok = True
    print("%-8s %-5s %-26s %12s %s" % ("частота", "bpm", "блоки", "макс. уход", "результат"))
    print("-" * 78)
    for rate, bpm, name, blocks in cases:
        res, err = check(rate, bpm, blocks, seconds=60)
        if err:
            ok = False
            print("%-8d %-5d %-26s %12s %s" % (rate, bpm, name, "-", "ОШИБКА: " + err))
            continue
        worst, worst_k, count = res
        good = worst == 0
        ok = ok and good
        print("%-8d %-5d %-26s %12d %s" % (
            rate, bpm, name, worst,
            "сэмпл в сэмпл (%d шагов)" % count if good else "РАСХОЖДЕНИЕ на шаге %d" % worst_k))

    print()
    print("ИТОГ:", "часы совпадают с эталоном экспорта" if ok else "ЕСТЬ РАСХОЖДЕНИЯ")
    return 0 if ok else 1


if __name__ == "__main__":
    # В pipe (CI, лог) Windows отдаёт stdout в ANSI-кодировке — русский
    # текст превращается в кракозябры. Консоль и так в UTF-8.
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
