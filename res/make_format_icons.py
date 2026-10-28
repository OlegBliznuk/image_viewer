# file-type icons (res/fmt_<ext>.ico, preview sheet res/fmt_preview.png): drawn at 1024 px and reduced from 40 px up, on the pixel grid below so the text stays crisp
import os
from PIL import Image, ImageChops, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = r'C:\Windows\Fonts\segoeuib.ttf'
SMALL_FONT = r'C:\Windows\Fonts\tahomabd.ttf'
# jpg, tga and tif take the label colours of ACDSee's icons, cr3 the colour of its CR2 icon
COLORS = {'exr': (244, 149, 255), 'png': (166, 30, 34), 'jpg': (128, 128, 0), 'hdr': (250, 216, 72), 'bmp': (31, 78, 158), 'tga': (0, 128, 128), 'cr3': (177, 226, 255), 'gif': (30, 123, 58), 'webp': (124, 203, 90), 'svg': (74, 74, 74), 'heic': (52, 50, 140), 'tif': (0, 0, 128), 'tx': (230, 120, 20), 'pic': (200, 200, 200)}
FOLD = (253, 255, 255, 255)
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
S = 1024

# 5 px tall pixel font, 1 px apart; W3 is the narrow W for WEBP at 16 px
GLYPHS = {
    'A': ['.#.', '#.#', '###', '#.#', '#.#'], 'B': ['##.', '#.#', '##.', '#.#', '##.'], 'C': ['.##', '#..', '#..', '#..', '.##'],
    'D': ['##.', '#.#', '#.#', '#.#', '##.'], 'E': ['###', '#..', '##.', '#..', '###'], 'F': ['###', '#..', '##.', '#..', '#..'],
    'G': ['.##.', '#...', '#.##', '#..#', '.##.'], 'H': ['#.#', '#.#', '###', '#.#', '#.#'], 'I': ['###', '.#.', '.#.', '.#.', '###'],
    'J': ['..#', '..#', '..#', '#.#', '.#.'], 'M': ['#...#', '##.##', '#.#.#', '#...#', '#...#'], 'N': ['#..#', '##.#', '#.##', '#..#', '#..#'],
    'P': ['##.', '#.#', '##.', '#..', '#..'], 'R': ['##.', '#.#', '##.', '#.#', '#.#'], 'T': ['###', '.#.', '.#.', '.#.', '.#.'],
    'W': ['#...#', '#...#', '#.#.#', '##.##', '#...#'], 'W3': ['#.#', '#.#', '#.#', '###', '#.#'], 'X': ['#.#', '#.#', '.#.', '#.#', '#.#'],
    '3': ['##.', '..#', '.#.', '..#', '##.'],
    'S': ['.##', '#..', '.#.', '..#', '##.'], 'V': ['#.#', '#.#', '#.#', '#.#', '.#.'],
}


def shade(c, k):
    return tuple(int(v * k) for v in c[:3]) + (255,)


def text_color(c):
    return (43, 43, 43, 255) if (0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]) / 255 > 0.6 else (255, 255, 255, 255)


def master(ext):
    body_c = COLORS[ext] + (255,)
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    h = S * 0.92
    w = h * 0.783
    x0, y0 = (S - w) / 2, (S - h) / 2
    x1, y1 = x0 + w, y0 + h
    r, f = 0.1 * w, 0.36 * w
    body = Image.new('L', (S, S), 0)
    d = ImageDraw.Draw(body)
    d.rounded_rectangle((x0, y0, x1, y1), radius=r, fill=255)
    d.polygon([(x1 - f, y0 - 1), (x1 + 1, y0 - 1), (x1 + 1, y0 + f)], fill=0)
    img.paste(Image.new('RGBA', (S, S), body_c), (0, 0), body)
    shadow = Image.new('L', (S, S), 0)
    ImageDraw.Draw(shadow).polygon([(x1 - f, y0 + f), (x1, y0 + f), (x1, y0 + 2 * f)], fill=255)
    img.paste(Image.new('RGBA', (S, S), shade(body_c, 0.85)), (0, 0), ImageChops.darker(shadow, body))
    ImageDraw.Draw(img).polygon([(x1 - f, y0), (x1 - f, y0 + f), (x1, y0 + f)], fill=FOLD)
    # proportions of the reference: x-height 12.3% of the height, left edge at 18.6% of the width, baseline at 82.3%; long extensions shrink to end by 90%
    ref = ImageFont.truetype(FONT, 100)
    xh = -ref.getbbox('x', anchor='ls')[1]
    text = '.' + ext
    size = min(100 * 0.123 * h / xh, 100 * (0.9 - 0.186) * w / ref.getlength(text))
    font = ImageFont.truetype(FONT, int(size))
    ImageDraw.Draw(img).text((x0 + 0.186 * w, y0 + 0.823 * h), text, font=font, fill=text_color(body_c), anchor='ls')
    return img


def page(n, x0, x1, y0, y1, f, body_c):
    img = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    shadow_c = shade(body_c, 0.85)
    fx = x1 - f + 1
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            dx, dy = x - fx, y - y0
            if dx >= 0 and dy < f:
                if dx <= dy:
                    img.putpixel((x, y), FOLD)
                continue
            if dx >= 0 and f <= dy < 2 * f and dx >= dy - f:
                img.putpixel((x, y), shadow_c)
                continue
            img.putpixel((x, y), body_c)
    for corner in ((x0, y0), (x0, y1), (x1, y1)):
        img.putpixel(corner, (0, 0, 0, 0))
    return img


def pixel_text(img, label, cx, top, color):
    glyphs = [GLYPHS[g] for g in label]
    width = sum(len(g[0]) for g in glyphs) + len(glyphs) - 1
    x = cx - width // 2
    for g in glyphs:
        for gy, row in enumerate(g):
            for gx, ch in enumerate(row):
                if ch == '#':
                    img.putpixel((x + gx, top + gy), color)
        x += len(g[0]) + 1


def small(ext, n):
    body_c = COLORS[ext] + (255,)
    color = text_color(body_c)
    label = ext.upper()
    if n == 16:
        img = page(16, 0, 14, 0, 15, 4, body_c)
        pixel_text(img, ['W3', 'E', 'B', 'P'] if ext == 'webp' else list(label), 7, 9, color)
        return img
    if n == 20:
        img = page(20, 1, 18, 0, 19, 5, body_c)
        pixel_text(img, list(label), 10, 11, color)
        return img
    # 24 and 32: Tahoma Bold drawn at its real size, so FreeType hints it to the pixel grid
    f = 6 if n == 24 else 8
    img = page(n, 1, n - 2, 0, n - 1, f, body_c)
    width, size = n - 4, n
    while size > 6:
        font = ImageFont.truetype(SMALL_FONT, size)
        if font.getlength(label) <= width and -font.getbbox(label, anchor='ls')[1] <= n * 0.36:
            break
        size -= 1
    ImageDraw.Draw(img).text(((n - 1) / 2, round(n * 0.83)), label, font=font, fill=color, anchor='ms')
    return img


def main():
    icons = {}
    for ext in COLORS:
        m = master(ext)
        frames = [small(ext, n) if n <= 32 else m.resize((n, n), Image.LANCZOS) for n in SIZES]
        frames[-1].save(os.path.join(HERE, 'fmt_%s.ico' % ext), format='ICO', sizes=[(n, n) for n in SIZES], append_images=frames[:-1])
        icons[ext] = frames
    sheet = Image.new('RGBA', (len(icons) * 140, 140 + 48 + 40), (255, 255, 255, 255))
    for i, (ext, frames) in enumerate(icons.items()):
        x = i * 140
        sheet.alpha_composite(frames[SIZES.index(128)], (x + 6, 4))
        sheet.alpha_composite(frames[SIZES.index(48)], (x + 6, 140))
        sheet.alpha_composite(frames[SIZES.index(32)], (x + 60, 148))
        sheet.alpha_composite(frames[SIZES.index(24)], (x + 98, 152))
        sheet.alpha_composite(frames[SIZES.index(20)], (x + 60, 196))
        sheet.alpha_composite(frames[SIZES.index(16)], (x + 90, 200))
    sheet.save(os.path.join(HERE, 'fmt_preview.png'))
    print('wrote', len(icons), 'icons')


if __name__ == '__main__':
    main()
