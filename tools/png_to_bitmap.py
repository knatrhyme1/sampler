"""Минимальный PNG-декодер (только stdlib) + генератор 1-битного PROGMEM
массива для Adafruit_GFX drawBitmap(). Нужен, потому что pip недоступен в
песочнице инструмента и PIL не установлен.

Использование:
    python png_to_bitmap.py <input.png> <out_width> <out_height> \
        <src_x> <src_y> <src_w> <src_h> <threshold> <out.h> <array_name>

src_x/y/w/h — прямоугольник исходного изображения, который масштабируется
до out_width x out_height методом ближайшего соседа (без внешних либ).
threshold — порог альфа-канала (0..255): пиксель считается "включённым",
если альфа >= threshold.
"""
import struct
import sys
import zlib


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    width = height = bitdepth = colortype = None
    idat = bytearray()
    palette = None
    trns = None
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        ctype = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + length]
        if ctype == b"IHDR":
            width, height, bitdepth, colortype = struct.unpack(
                ">IIBB", chunk[:10]
            )
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"PLTE":
            palette = chunk
        elif ctype == b"tRNS":
            trns = chunk
        elif ctype == b"IEND":
            break
        pos += 8 + length + 4  # length + type + data + crc

    raw = zlib.decompress(bytes(idat))

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colortype]
    bpp = max(1, (channels * bitdepth) // 8)
    stride = (width * channels * bitdepth + 7) // 8

    out = bytearray(stride * height)
    src_off = 0
    prev = bytearray(stride)
    for y in range(height):
        filt = raw[src_off]
        src_off += 1
        line = bytearray(raw[src_off : src_off + stride])
        src_off += stride
        if filt == 0:
            pass
        elif filt == 1:  # Sub
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + a) & 0xFF
        elif filt == 2:  # Up
            for i in range(stride):
                b = prev[i]
                line[i] = (line[i] + b) & 0xFF
        elif filt == 3:  # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
        elif filt == 4:  # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        else:
            raise ValueError("unknown filter %d" % filt)
        out[y * stride : (y + 1) * stride] = line
        prev = line

    assert bitdepth == 8, "only 8-bit PNG supported by this script"

    def get_rgba(x, y):
        base = y * stride + x * channels
        if colortype == 6:
            return out[base], out[base + 1], out[base + 2], out[base + 3]
        if colortype == 2:
            return out[base], out[base + 1], out[base + 2], 255
        if colortype == 0:
            v = out[base]
            return v, v, v, 255
        if colortype == 4:
            v = out[base]
            return v, v, v, out[base + 1]
        if colortype == 3:
            idx = out[base]
            r, g, b = palette[idx * 3 : idx * 3 + 3]
            a = trns[idx] if trns and idx < len(trns) else 255
            return r, g, b, a
        raise ValueError("unsupported colortype %d" % colortype)

    return width, height, get_rgba


def main():
    (
        in_path,
        out_w,
        out_h,
        sx,
        sy,
        sw,
        sh,
        threshold,
        out_path,
        array_name,
    ) = sys.argv[1:11]
    out_w, out_h = int(out_w), int(out_h)
    sx, sy, sw, sh = int(sx), int(sy), int(sw), int(sh)
    threshold = int(threshold)

    width, height, get_rgba = read_png(in_path)
    print(f"source: {width}x{height}")

    bits = [[0] * out_w for _ in range(out_h)]
    for oy in range(out_h):
        srcy = sy + (oy * sh) // out_h
        for ox in range(out_w):
            srcx = sx + (ox * sw) // out_w
            r, g, b, a = get_rgba(srcx, srcy)
            lum = 0.299 * r + 0.587 * g + 0.114 * b
            on = a >= threshold and lum >= 96
            bits[oy][ox] = 1 if on else 0

    # Лёгкая дилатация (+1 сосед по горизонтали) — тонкие штрихи хатча
    # иначе пропадают при масштабировании превью в симуляторе Wokwi.
    dilated = [row[:] for row in bits]
    for y in range(out_h):
        for x in range(out_w):
            if bits[y][x] and x + 1 < out_w:
                dilated[y][x + 1] = 1
    bits = dilated

    row_bytes = (out_w + 7) // 8
    data = bytearray(row_bytes * out_h)
    for y in range(out_h):
        for x in range(out_w):
            if bits[y][x]:
                data[y * row_bytes + (x // 8)] |= 0x80 >> (x % 8)

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(
            f"// Автосгенерировано tools/png_to_bitmap.py из smplr-logo.png\n"
        )
        f.write(f"// {out_w}x{out_h}, 1 бит/пиксель, MSB первым в байте.\n")
        f.write(
            f"constexpr uint16_t {array_name}Width = {out_w};\n"
            f"constexpr uint16_t {array_name}Height = {out_h};\n"
        )
        f.write(f"const uint8_t {array_name}[] PROGMEM = {{\n")
        for i in range(0, len(data), 16):
            row = data[i : i + 16]
            f.write("  " + ",".join(f"0x{b:02X}" for b in row) + ",\n")
        f.write("};\n")

    on_count = sum(sum(r) for r in bits)
    print(
        f"wrote {out_path}: {out_w}x{out_h}, {len(data)} bytes, "
        f"{on_count}/{out_w*out_h} pixels on ({100*on_count/(out_w*out_h):.1f}%)"
    )


if __name__ == "__main__":
    main()
