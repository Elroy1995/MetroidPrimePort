#!/usr/bin/python3
"""Checks the importer's image code (platform/port_remastered_image.cpp) against Pillow.

    port_remastered_image_check.py <port_remastered_image_tool> <image>...

For each image: the resize against Pillow's Lanczos, the BC7 and BC5 .dds as Pillow decodes
them against the source, and the RGBA8 and CMPR TXTRs decoded here. Prints PSNR in dB; exits 1
if one falls under its floor. Needs Pillow and numpy; the images are the tester's own."""
import os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

FLOOR = {'resize': 45.0, 'bc7': 36.0, 'bc5': 38.0, 'rgba8': 99.0, 'cmpr': 26.0}


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse)


def split(img):
    return Image.merge('RGBA', [c.resize((img.width // 2, img.height // 2), Image.LANCZOS) for c in img.split()])


def rgba8_top(d):
    fmt, w, h, n = struct.unpack_from('>IHHI', d)
    assert fmt == 9
    a = np.frombuffer(d, np.uint8, w * h * 4, 12).reshape(h // 4, w // 4, 2, 16, 2)
    out = np.empty((h // 4, w // 4, 16, 4), np.uint8)
    out[..., 3], out[..., 0] = a[:, :, 0, :, 0], a[:, :, 0, :, 1]
    out[..., 1], out[..., 2] = a[:, :, 1, :, 0], a[:, :, 1, :, 1]
    return out.reshape(h // 4, w // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, 4)


def cmpr_top(d):
    fmt, w, h, n = struct.unpack_from('>IHHI', d)
    assert fmt == 10
    b = np.frombuffer(d, np.uint8, w * h // 2, 12).reshape(-1, 8).astype(np.int32)
    v = [(b[:, 0] << 8) | b[:, 1], (b[:, 2] << 8) | b[:, 3]]
    e = []
    for x in v:
        r, g, bl = x >> 11, (x >> 5) & 63, x & 31
        e.append(np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2)], -1))
    four = (v[0] > v[1])[:, None]
    pal = np.stack([e[0], e[1], np.where(four, (5 * e[0] + 3 * e[1]) >> 3, (e[0] + e[1]) >> 1),
                    np.where(four, (3 * e[0] + 5 * e[1]) >> 3, (e[0] + e[1]) >> 1)], 1)
    idx = np.stack([(b[:, 4 + r] >> s) & 3 for r in range(4) for s in (6, 4, 2, 0)], 1)
    rgb = np.take_along_axis(pal, idx[..., None].repeat(3, 2), 1)
    return rgb.reshape(h // 8, w // 8, 2, 2, 4, 4, 3).transpose(0, 2, 4, 1, 3, 5, 6).reshape(h, w, 3)


def main():
    tool, bad = sys.argv[1], 0
    tmp = tempfile.mkdtemp()
    raw, out = os.path.join(tmp, 'in.rgba'), os.path.join(tmp, 'out')

    def run(op, img, *more):
        open(raw, 'wb').write(img.tobytes())
        subprocess.run([tool, op, str(img.width), str(img.height), raw, out + more[0] if more else out]
                       + [str(v) for v in more[1:]], check=True, stdout=subprocess.DEVNULL)

    for path in sys.argv[2:]:
        img = Image.open(path).convert('RGBA')
        w, h = max(img.width // 8 * 8, 8), max(img.height // 8 * 8, 8)
        img = img.crop((0, 0, w, h))
        src = np.asarray(img)
        got = {}
        run('resize', img, '', w // 2, h // 2)
        got['resize'] = psnr(np.frombuffer(open(out, 'rb').read(), np.uint8).reshape(h // 2, w // 2, 4),
                             np.asarray(split(img)))
        run('bc7', img, '.dds')
        got['bc7'] = psnr(np.asarray(Image.open(out + '.dds').convert('RGBA')), src)
        run('bc5', img, '.dds')
        got['bc5'] = psnr(np.asarray(Image.open(out + '.dds').convert('RGBA'))[..., :2], src[..., :2])
        run('rgba8', img)
        got['rgba8'] = psnr(rgba8_top(open(out, 'rb').read()), src)
        run('cmpr', img)
        got['cmpr'] = psnr(cmpr_top(open(out, 'rb').read()), src[..., :3])
        low = [k for k in got if got[k] < FLOOR[k]]
        bad += bool(low)
        print('%-44s %s%s' % (os.path.basename(path)[:44], '  '.join('%s %5.1f' % (k, got[k]) for k in got),
                              '   LOW: ' + ','.join(low) if low else ''))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
