"""Разовый инструмент для снятия реальной MIDI-раскладки Akai MPK mini.

Не часть прошивки/движка — запускается на ПК с реально подключённым
контроллером, чтобы увидеть, какие note/CC номера шлёт каждый физический
элемент. Результат используется для заполнения docs/mpk-mapping.md.

Использование:
    python tools/midi_logger.py
"""

import sys
import time

import mido

PORT_SUBSTR = "mpk"

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(note: int) -> str:
    return f"{NOTE_NAMES[note % 12]}{note // 12 - 1}"


def find_port() -> str:
    names = mido.get_input_names()
    for name in names:
        if PORT_SUBSTR in name.lower():
            return name
    raise SystemExit(
        f"MIDI-порт с '{PORT_SUBSTR}' в имени не найден. Доступные порты: {names}"
    )


def describe(msg: "mido.Message") -> str:
    if msg.type in ("note_on", "note_off"):
        return (
            f"{msg.type:<9} ch={msg.channel + 1} note={msg.note:3d} "
            f"({note_name(msg.note)}) vel={msg.velocity}"
        )
    if msg.type == "control_change":
        return f"control_change ch={msg.channel + 1} cc={msg.control:3d} value={msg.value}"
    if msg.type == "pitchwheel":
        return f"pitchwheel     ch={msg.channel + 1} value={msg.pitch}"
    if msg.type == "program_change":
        return f"program_change ch={msg.channel + 1} program={msg.program}"
    if msg.type == "aftertouch":
        return f"aftertouch     ch={msg.channel + 1} value={msg.value}"
    if msg.type == "polytouch":
        return f"polytouch      ch={msg.channel + 1} note={msg.note} value={msg.value}"
    return str(msg)


def main() -> None:
    port_name = find_port()
    print(f"Слушаю порт: {port_name}", flush=True)
    print("Жми клавиши/пэды/крутилки на MPK. Ctrl+C — выход.\n", flush=True)
    start = time.time()
    with mido.open_input(port_name) as port:
        try:
            for msg in port:
                t = time.time() - start
                print(f"{t:8.3f}s  {describe(msg)}", flush=True)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
