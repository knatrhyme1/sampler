"""Собирает WAV из сохранённого текста Serial Monitor — запасной путь к экспорту.

Обычный путь — закладка из tools/export_bookmarklet.html: она сама ловит
экспорт на странице Wokwi и скачивает файл. Этот скрипт нужен, когда файл
скачать не удалось, а текст в мониторе остался: выдели его, скопируй,
сохрани в .txt и скорми этому скрипту.

Запуск:

    python tools/wav_from_serial.py monitor.txt            # рядом появится .wav
    python tools/wav_from_serial.py monitor.txt -o out.wav

На Windows можно просто перетащить .txt на tools/wav_from_serial.bat.

Формат строк описан в firmware/pattern_export.h. Скрипт проверяет ту же
CRC32, что и прошивка: если хоть один байт потерялся, файл не сохраняется.
"""

import argparse
import base64
import binascii
import re
import sys
from pathlib import Path

BEGIN = re.compile(
    r"EXPORT-BEGIN id=([0-9a-f]{8}) file=(\S+) bytes=(\d+) lines=(\d+)(?: bpl=(\d+))?"
)
LINE = re.compile(r"^E:(\d+):([A-Za-z0-9+/=]+)\s*$", re.MULTILINE)
END = "EXPORT-END id={} crc32="
ABORT = "EXPORT-ABORT id={}"
DEFAULT_BYTES_PER_LINE = 57  # сборки до 20.09.2026 не печатали bpl


def read_text(path):
    # Монитор мог сохраниться в чём угодно: пробуем UTF-8, потом cp1251.
    raw = Path(path).read_bytes()
    for encoding in ("utf-8", "cp1251", "latin-1"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def collect(text):
    """Находит последний экспорт в тексте и собирает его байты."""
    begins = list(BEGIN.finditer(text))
    if not begins:
        raise SystemExit(
            "в файле нет строки EXPORT-BEGIN — это не текст монитора с экспортом"
        )
    head = begins[-1]
    export_id, name, total_bytes, total_lines = head.group(1), head.group(2), int(
        head.group(3)
    ), int(head.group(4))
    bytes_per_line = int(head.group(5) or DEFAULT_BYTES_PER_LINE)
    region = text[head.end():]

    if ABORT.format(export_id) in region:
        raise SystemExit("этот экспорт отменили на устройстве — данных нет")

    parts = [None] * total_lines
    for match in LINE.finditer(region):
        index = int(match.group(1))
        if 0 <= index < total_lines and parts[index] is None:
            parts[index] = match.group(2)

    missing = [i for i, part in enumerate(parts) if part is None]
    if missing:
        raise SystemExit(
            f"не хватает {len(missing)} строк из {total_lines} "
            f"(первая пропущенная — {missing[0]}). "
            "Текст монитора неполный: запусти экспорт заново с нажатой закладкой."
        )

    data = bytearray()
    for index, part in enumerate(parts):
        chunk = base64.b64decode(part, validate=True)
        expected = bytes_per_line if index < total_lines - 1 else total_bytes - len(data)
        if len(chunk) != expected:
            raise SystemExit(
                f"строка {index} неожиданной длины: {len(chunk)} байт вместо {expected}"
            )
        data += chunk

    if len(data) != total_bytes:
        raise SystemExit(f"собралось {len(data)} байт вместо {total_bytes}")

    crc_match = re.search(END.format(export_id) + "([0-9a-f]{8})", region)
    if not crc_match:
        raise SystemExit(
            "нет строки EXPORT-END — экспорт в этом тексте не закончился"
        )
    crc_expected = crc_match.group(1)
    crc_actual = f"{binascii.crc32(bytes(data)) & 0xFFFFFFFF:08x}"
    if crc_actual != crc_expected:
        raise SystemExit(
            f"контрольная сумма не сошлась: {crc_actual} вместо {crc_expected}. "
            "Файл повреждён, сохранять не буду."
        )

    return name, bytes(data)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("log", help="текстовый файл с выводом Serial Monitor")
    parser.add_argument("-o", "--out", help="куда положить WAV (по умолчанию — рядом с логом)")
    args = parser.parse_args()

    name, data = collect(read_text(args.log))
    out = Path(args.out) if args.out else Path(args.log).with_name(name)
    out.write_bytes(data)
    print(f"готово: {out} ({len(data) / 1024:.0f} КБ), контрольная сумма сошлась")


if __name__ == "__main__":
    main()
