#!/usr/bin/env python3
"""生成 DesktopLayoutNative 的多尺寸图标，并打包成可链接的 COFF 资源对象。

用法：python assets/app_icon.py
产物：assets/app_icon.ico（16/24/32/48/64/256，256 用 PNG）
      assets/app_icon.obj（只含 .rsrc 段，供 zig cc 直接链接）

设计：深色圆角底（#232325，描边 #3a3a3c）+ 2x2 方块网格，左上角一块用界面
强调蓝 #3aa0ff；16px 下仍是四个 4px 实心块，不依赖细线。依赖 Pillow。
"""
import os
import struct
import sys

from PIL import Image, ImageDraw

SIZES = [16, 24, 32, 48, 64, 256]
SS = 8
BG = (0x23, 0x23, 0x25, 255)
BORDER = (0x3a, 0x3a, 0x3c, 255)
LIGHT = (0xd9, 0xd9, 0xdd, 255)
ACCENT = (0x3a, 0xa0, 0xff, 255)
RT_ICON = 3
RT_GROUP_ICON = 14
LANG = 0x0409


def draw(size):
    s = size * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = max(1, int(round(s * 0.0625)))
    rad = max(2, int(round(s * 0.22)))
    bw = max(1, int(round(s * 0.012)))
    d.rounded_rectangle([pad, pad, s - pad - 1, s - pad - 1], radius=rad,
                        fill=BG, outline=BORDER, width=bw)
    inner = max(2, int(round(s * 0.17)))
    gap = max(1, int(round(s * 0.055)))
    tile = (s - inner * 2 - gap) // 2
    trad = max(1, int(round(tile * 0.18)))
    for row in range(2):
        for col in range(2):
            x = inner + col * (tile + gap)
            y = inner + row * (tile + gap)
            color = ACCENT if (row == 0 and col == 0) else LIGHT
            d.rounded_rectangle([x, y, x + tile - 1, y + tile - 1],
                                radius=trad, fill=color)
    out = img.resize((size, size), Image.LANCZOS)
    px = out.load()
    for y in range(size):
        for x in range(size):
            if px[x, y][3] < 8:
                px[x, y] = (0, 0, 0, 0)
    return out


def bmp_bytes(img):
    w, h = img.size
    px = img.load()
    xor = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
    mask_row = ((w + 31) // 32) * 4
    and_mask = bytes(mask_row * h)
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0,
                         len(xor) + len(and_mask), 0, 0, 0, 0)
    return header + bytes(xor) + and_mask


def png_bytes(img):
    import io
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_ico(path):
    images = []
    for size in SIZES:
        img = draw(size)
        payload = png_bytes(img) if size >= 256 else bmp_bytes(img)
        images.append((size, payload))
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, payload in images:
        w = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(payload), offset)
        offset += len(payload)
    for _, payload in images:
        out += payload
    with open(path, "wb") as f:
        f.write(out)
    return images


def dir_entry(name, target, is_dir):
    return struct.pack("<II", name, (0x80000000 | target) if is_dir else target)


def write_obj(path, icons):
    # icons: [(边长, 图像字节)] -> (宽, 高, bytesInRes, 数据)
    icons = [(size, size, len(data), data) for size, data in icons]
    blobs = [data for _, _, _, data in icons]
    group = struct.pack("<HHH", 0, 1, len(icons))
    for idx, (w, h, size, _) in enumerate(icons, start=1):
        group += struct.pack("<BBBBHHIH", 0 if w >= 256 else w,
                             0 if h >= 256 else h, 0, 0, 1, 32, size, idx)
    blobs.append(group)

    n = len(icons)
    root_off = 0
    root_size = 16 + 2 * 8
    d3_off = root_size
    d3_size = 16 + n * 8
    d14_off = d3_off + d3_size
    d14_size = 16 + 8
    lang_off = d14_off + d14_size
    lang_size = (n + 1) * 24
    data_off = lang_off + lang_size
    data_size = (n + 1) * 16
    blob_off = data_off + data_size

    lang_dirs = {}
    off = lang_off
    for i in range(1, n + 1):
        lang_dirs[("icon", i)] = off
        off += 24
    lang_dirs[("group", 101)] = off

    data_entries = {}
    off = data_off
    for i in range(1, n + 1):
        data_entries[("icon", i)] = off
        off += 16
    data_entries[("group", 101)] = off

    blob_pos = {}
    off = blob_off
    for i in range(1, n + 1):
        blob_pos[("icon", i)] = off
        off += len(blobs[i - 1])
    blob_pos[("group", 101)] = off
    total = off + len(blobs[-1])

    sec = bytearray(total)
    struct.pack_into("<IIHHHH", sec, root_off, 0, 0, 0, 0, 0, 2)
    struct.pack_into("<II", sec, root_off + 16, RT_ICON, 0x80000000 | d3_off)
    struct.pack_into("<II", sec, root_off + 24, RT_GROUP_ICON, 0x80000000 | d14_off)
    struct.pack_into("<IIHHHH", sec, d3_off, 0, 0, 0, 0, 0, n)
    for i in range(1, n + 1):
        struct.pack_into("<II", sec, d3_off + 16 + (i - 1) * 8,
                         i, 0x80000000 | lang_dirs[("icon", i)])
    struct.pack_into("<IIHHHH", sec, d14_off, 0, 0, 0, 0, 0, 1)
    struct.pack_into("<II", sec, d14_off + 16, 101,
                     0x80000000 | lang_dirs[("group", 101)])
    for key, base in lang_dirs.items():
        struct.pack_into("<IIHHHH", sec, base, 0, 0, 0, 0, 0, 1)
        struct.pack_into("<II", sec, base + 16, LANG, data_entries[key])
    for i in range(1, n + 1):
        struct.pack_into("<IIII", sec, data_entries[("icon", i)],
                         blob_pos[("icon", i)], len(blobs[i - 1]), 0, 0)
    struct.pack_into("<IIII", sec, data_entries[("group", 101)],
                     blob_pos[("group", 101)], len(blobs[-1]), 0, 0)
    for i in range(1, n + 1):
        p = blob_pos[("icon", i)]
        sec[p:p + len(blobs[i - 1])] = blobs[i - 1]
    sec[blob_pos[("group", 101)]:] = blobs[-1]

    raw = bytes(sec)
    nrel = n + 1
    reloc_off = 20 + 40 + len(raw)
    sym_off = reloc_off + nrel * 10
    hdr = struct.pack("<HHIIIHH", 0x8664, 1, 0, sym_off, 1, 0, 0)
    sec_hdr = struct.pack("<8sIIIIIIHHI", b".rsrc\0\0\0", len(raw), 0, len(raw),
                          60, reloc_off, 0, nrel, 0, 0x40300040)
    relocs = bytearray()
    for de_off in data_entries.values():
        relocs += struct.pack("<IIH", de_off, 0, 0x0003)  # ADDR32NB
    sym = struct.pack("<8sIhHBB", b".rsrc\0\0\0", 0, 1, 0, 3, 0)
    out = hdr + sec_hdr + raw + relocs + sym + struct.pack("<I", 4)
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def main():
    base = os.path.dirname(os.path.abspath(__file__))
    ico = os.path.join(base, "app_icon.ico")
    obj = os.path.join(base, "app_icon.obj")
    icons = write_ico(ico)
    size_obj = write_obj(obj, icons)
    with Image.open(ico) as im:
        print("ico=%d bytes sizes=%s" % (os.path.getsize(ico),
                                          sorted(im.info.get("sizes", []))))
    print("obj=%d bytes" % size_obj)


if __name__ == "__main__":
    sys.exit(main())
