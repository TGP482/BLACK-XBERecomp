# Flat PE of an XBE for Ghidra: the image at its load address, recompiler functions and config names as exports.
# usage, from the game repo after a build: python sdk/tools/symbols/xbe2pe.py config/game.toml game.exe
import re
import struct
import sys
import tomllib
from pathlib import Path


def load(path, cfg):
    t = tomllib.loads(path.read_text())
    for inc in t.get('includes', []):
        load(path.parent / inc, cfg)
    for k in ('xbe', 'functions', 'symbols'):
        cfg.setdefault(k, {}).update(t.get(k, {}))
    return cfg


cfg_path, out = Path(sys.argv[1]), Path(sys.argv[2])
cfg = load(cfg_path, {})
root = cfg_path.parent.parent
d = (root / cfg['xbe']['path']).read_bytes()
u32 = lambda o: struct.unpack_from('<I', d, o)[0]
# XBE header: base, headers size, image size; the entry is xored with the retail key, else the debug key
base, hsize, isize = u32(0x104), u32(0x108), u32(0x10C)
entry = u32(0x128) ^ 0xA8FC57AB
if not base <= entry < base + isize:
    entry = u32(0x128) ^ 0x94859D4B

img = bytearray(isize)
img[:hsize] = d[:hsize]
# section headers are 56 bytes, virtual address, virtual size, raw address and raw size from +4
for i in range(u32(0x11C)):
    va, vs, raw, rs = struct.unpack_from('<IIII', d, u32(0x120) - base + i * 56 + 4)
    n = min(vs, rs)
    img[va - base:va - base + n] = d[raw:raw + n]

names = {}
mapping = root / cfg['xbe']['out'] / 'x86_func_mapping.cpp'
if mapping.exists():
    for a in re.findall(r'\{0x([0-9A-F]+)u, sub_', mapping.read_text()):
        names[int(a, 16)] = 'sub_%08X' % int(a, 16)
for va, name in cfg['functions'].items():
    names[int(va, 0)] = name.replace('::', '_')
for name, va in cfg['symbols'].items():
    if va:
        names[va] = name

exp = sorted(names)
nm = [names[a].encode() for a in exp]
order = sorted(range(len(exp)), key=lambda i: nm[i])
# export directory (40 bytes), address table, name pointers, ordinals and names, page aligned after the image
ert = (isize + 0xFFF) & ~0xFFF
eat = ert + 40
npt = eat + 4 * len(exp)
ot = npt + 4 * len(exp)
st = ot + 2 * len(exp)
strs, off = bytearray(b'image.exe\0'), {}
for i in range(len(exp)):
    off[i] = st + len(strs)
    strs += nm[i] + b'\0'
edata = struct.pack('<IIHHIIIIIII', 0, 0, 0, 0, st, 1, len(exp), len(exp), eat, npt, ot)
edata += b''.join(struct.pack('<I', a - base) for a in exp)
edata += b''.join(struct.pack('<I', off[i]) for i in order)
edata += b''.join(struct.pack('<H', i) for i in order)
edata += strs
esz = (len(edata) + 0xFFF) & ~0xFFF

opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII', 0x10B, 14, 0, isize, 0, 0, entry - base, 0x1000, 0x1000, base,
                  0x1000, 0x1000, 4, 0, 0, 0, 4, 0, 0, ert + esz, 0x1000, 0, 2, 0, 0x100000, 0x1000, 0x100000, 0x1000,
                  0, 16)
opt += struct.pack('<II', ert, len(edata)) + bytes(8 * 15)
coff = struct.pack('<HHIIIHH', 0x14C, 2, 0, 0, 0, len(opt), 0x0102)
flat = struct.pack('<8sIIIIIIHHI', b'.flat', isize - 0x1000, 0x1000, isize - 0x1000, 0x1000, 0, 0, 0, 0, 0xE0000060)
exps = struct.pack('<8sIIIIIIHHI', b'.edata', len(edata), ert, esz, ert, 0, 0, 0, 0, 0x40000040)
dos = bytearray(0x40)
dos[:2] = b'MZ'
struct.pack_into('<I', dos, 0x3C, 0x40)
head = bytes(dos) + b'PE\0\0' + coff + opt + flat + exps

f = bytearray(ert + esz)
f[0x1000:isize] = img[0x1000:]
f[:len(head)] = head
f[ert:ert + len(edata)] = edata
out.write_bytes(f)
print(out, len(exp), 'exports')
