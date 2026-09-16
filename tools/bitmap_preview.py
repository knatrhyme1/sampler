"""Рендерит .h с PROGMEM-массивом (1 бит/пиксель) обратно в PNG для
визуальной проверки — pure stdlib (zlib + struct), без PIL."""
import re
import struct
import sys
import zlib


def write_png_gray(path, width, height, pixels):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(
            ">I", zlib.crc32(c) & 0xFFFFFFFF
        )

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(pixels[y * width : (y + 1) * width])
    idat = zlib.compress(bytes(raw), 9)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def main():
    h_path, out_path = sys.argv[1], sys.argv[2]
    text = open(h_path, encoding="utf-8").read()
    w = int(re.search(r"Width = (\d+);", text).group(1))
    h = int(re.search(r"Height = (\d+);", text).group(1))
    bytes_hex = re.findall(r"0x([0-9A-Fa-f]{2})", text)
    data = bytes(int(b, 16) for b in bytes_hex)
    row_bytes = (w + 7) // 8
    pixels = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            byte = data[y * row_bytes + x // 8]
            bit = (byte >> (7 - (x % 8))) & 1
            pixels[y * w + x] = 255 if bit else 0
    write_png_gray(out_path, w, h, pixels)
    print(f"wrote {out_path} ({w}x{h})")


if __name__ == "__main__":
    main()
