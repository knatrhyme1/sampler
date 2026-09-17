"""WAV-ваншоты -> PROGMEM-массивы int16 для прошивки (только stdlib, без
numpy — как png_to_bitmap.py).

Каждый файл: стерео сводится в моно, ресэмплится в частоту звукового движка
(firmware/audio_engine.h, kAudioSampleRate) оконным sinc-фильтром, хвост
тишины обрезается, в конце — короткий fade-out, чтобы не щёлкало.
Понимает PCM 16/24/32 бит и float 32 (формат 3), лишние чанки (smpl, ID3,
LIST...) пропускает.

Использование:
    python wav_to_header.py <sample_rate> <out.h> <name>=<file.wav> [...]
"""
import math
import struct
import sys

SILENCE_THRESHOLD = 10 ** (-60 / 20)  # -60 dBFS
FADE_OUT_MS = 5
SINC_HALF_TAPS = 16


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: не RIFF/WAVE")
    fmt = None
    pcm = None
    i = 12
    while i + 8 <= len(data):
        cid = data[i:i + 4]
        size = struct.unpack("<I", data[i + 4:i + 8])[0]
        body = data[i + 8:i + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: реальный код формата в SubFormat
                fmt = (struct.unpack("<H", body[24:26])[0],) + fmt[1:]
        elif cid == b"data":
            pcm = body
        i += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        raise ValueError(f"{path}: нет fmt или data")

    code, channels, rate, _, block_align, bits = fmt
    frame_count = len(pcm) // block_align
    width = bits // 8
    if code == 3 and bits == 32:
        values = struct.unpack(f"<{frame_count * channels}f", pcm[:frame_count * block_align])
    elif code == 1 and bits == 16:
        values = [v / 32768 for v in
                  struct.unpack(f"<{frame_count * channels}h", pcm[:frame_count * block_align])]
    elif code == 1 and bits == 32:
        values = [v / 2147483648 for v in
                  struct.unpack(f"<{frame_count * channels}i", pcm[:frame_count * block_align])]
    elif code == 1 and bits == 24:
        values = []
        for k in range(frame_count * channels):
            b = pcm[k * width:k * width + 3]
            values.append(int.from_bytes(b, "little", signed=True) / 8388608)
    else:
        raise ValueError(f"{path}: формат {code}/{bits} бит не поддержан")

    mono = [sum(values[f * channels:(f + 1) * channels]) / channels for f in range(frame_count)]
    return mono, rate


def resample(src, src_rate, dst_rate):
    if src_rate == dst_rate:
        return list(src)
    ratio = dst_rate / src_rate
    cutoff = min(1.0, ratio)  # при понижении частоты фильтр срезает выше новой Найквиста
    out_len = int(len(src) * ratio)
    out = []
    for n in range(out_len):
        t = n / ratio
        center = int(math.floor(t))
        acc = 0.0
        for k in range(center - SINC_HALF_TAPS + 1, center + SINC_HALF_TAPS + 1):
            if k < 0 or k >= len(src):
                continue
            x = t - k
            arg = math.pi * x * cutoff
            sinc = 1.0 if abs(arg) < 1e-9 else math.sin(arg) / arg
            window = 0.5 + 0.5 * math.cos(math.pi * x / SINC_HALF_TAPS)  # Ханн
            acc += src[k] * cutoff * sinc * window
        out.append(acc)
    return out


def trim_and_fade(samples, rate):
    end = len(samples)
    while end > 0 and abs(samples[end - 1]) < SILENCE_THRESHOLD:
        end -= 1
    samples = samples[:end]
    fade = min(len(samples), int(rate * FADE_OUT_MS / 1000))
    for k in range(fade):
        samples[len(samples) - fade + k] *= 1.0 - (k + 1) / fade
    return samples


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    rate = int(sys.argv[1])
    out_path = sys.argv[2]
    entries = [arg.split("=", 1) for arg in sys.argv[3:]]

    lines = [
        "// Сгенерировано tools/wav_to_header.py — не править руками.",
        f"// Ваншоты по умолчанию: моно, {rate} Гц, int16.",
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        f"static const uint32_t kDefaultSamplesRate = {rate};",
        "",
    ]
    for name, path in entries:
        mono, src_rate = read_wav(path)
        samples = trim_and_fade(resample(mono, src_rate, rate), rate)
        ints = [max(-32768, min(32767, int(round(v * 32767)))) for v in samples]
        peak = max((abs(v) for v in ints), default=0)
        src_name = path.replace("\\", "/").split("/")[-1]
        lines.append(f"// {src_name}: {len(ints)} сэмплов, {len(ints) / rate * 1000:.0f} мс, "
                     f"пик {20 * math.log10(max(peak, 1) / 32767):.1f} dBFS")
        lines.append(f"static const int16_t {name}[] PROGMEM = {{")
        for k in range(0, len(ints), 16):
            lines.append("  " + ",".join(str(v) for v in ints[k:k + 16]) + ",")
        lines.append("};")
        lines.append(f"static const uint32_t {name}Length = {len(ints)};")
        lines.append("")
        print(f"{name}: {src_name} {src_rate}Hz -> {len(ints)} samples, peak {peak}")

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
