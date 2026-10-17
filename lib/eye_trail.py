# Paint one transparent frame per video frame. Each frame holds the whole
# eye trail, so ffmpeg can overlay it once instead of moving a dot per sample.
#
# stdin: one line per frame, "x y" in video pixels (center), or "-" when
# that frame has no sample yet.
# stdout: raw rgba, width * height * 4 bytes per frame, straight alpha.

import sys

def sprite(size, alpha):
    pic = bytearray(size * size * 4)
    cx = size / 2.0
    radius = size * 3.5 / 8.0
    for y in range(size):
        for x in range(size):
            d = ((x + 0.5 - cx) ** 2 + (y + 0.5 - cx) ** 2) ** 0.5
            if d <= radius - 0.5:
                cov = 1.0
            elif d >= radius + 0.5:
                cov = 0.0
            else:
                cov = radius + 0.5 - d
            a = int(round(255 * alpha * cov))
            if a <= 0:
                continue
            if a > 255:
                a = 255
            o = (y * size + x) * 4
            pic[o] = 255
            pic[o + 1] = 255
            pic[o + 2] = 255
            pic[o + 3] = a
    return pic

def blit(buf, w, spr, size, left, top, fw, fh):
    x0 = left if left > 0 else 0
    y0 = top if top > 0 else 0
    x1 = left + size if left + size < fw else fw
    y1 = top + size if top + size < fh else fh
    if x0 >= x1 or y0 >= y1:
        return None
    for y in range(y0, y1):
        sy = y - top
        row = (y * w + x0) * 4
        srow = (sy * size + (x0 - left)) * 4
        for x in range(x0, x1):
            sa = spr[srow + 3]
            if sa:
                da = buf[row + 3]
                if da == 0:
                    buf[row] = spr[srow]
                    buf[row + 1] = spr[srow + 1]
                    buf[row + 2] = spr[srow + 2]
                    buf[row + 3] = sa
                else:
                    sa_f = sa / 255.0
                    da_f = da / 255.0
                    oa = sa_f + da_f * (1.0 - sa_f)
                    dr = buf[row]
                    r = (255.0 * sa_f + dr * da_f * (1.0 - sa_f)) / oa
                    a = int(round(oa * 255.0))
                    if a > 255:
                        a = 255
                    ir = int(round(r))
                    buf[row] = ir
                    buf[row + 1] = ir
                    buf[row + 2] = ir
                    buf[row + 3] = a
            row += 4
            srow += 4
    return (x0, y0, x1, y1)

def wipe(buf, w, rect):
    x0, y0, x1, y1 = rect
    nbytes = (x1 - x0) * 4
    blank = bytes(nbytes)
    for y in range(y0, y1):
        o = (y * w + x0) * 4
        buf[o:o + nbytes] = blank

def main():
    fw = int(sys.argv[1])
    fh = int(sys.argv[2])
    size = int(sys.argv[3])
    fade = float(sys.argv[4])
    gap = int(sys.argv[5])
    count = int(sys.argv[6])
    if size < 1:
        size = 1
    dots = []
    scale = 0.5
    for _ in range(count):
        dots.append(sprite(size, scale))
        scale *= fade

    moves = []
    for line in sys.stdin:
        line = line.strip()
        if line == "" or line == "-":
            moves.append(None)
        else:
            xs, ys = line.split()
            moves.append((float(xs), float(ys)))

    buf = bytearray(fw * fh * 4)
    out = sys.stdout.buffer
    half = size / 2.0
    n = len(moves)
    for f in range(n):
        chosen = []
        seen = set()
        for i in range(count):
            src = f - i * gap
            if src < 0:
                continue
            pos = moves[src]
            if pos is None:
                continue
            key = (round(pos[0], 2), round(pos[1], 2))
            if key in seen:
                continue
            seen.add(key)
            left = int(round(pos[0] - half))
            top = int(round(pos[1] - half))
            chosen.append((i, left, top))
        dirty = []
        for i, left, top in reversed(chosen):
            rect = blit(buf, fw, dots[i], size, left, top, fw, fh)
            if rect is not None:
                dirty.append(rect)
        out.write(buf)
        for rect in dirty:
            wipe(buf, fw, rect)

if __name__ == "__main__":
    main()
