"""Export the approved transparent master. Requires Python and Pillow.
Generated files are committed; normal application builds do not need Python.
"""
from pathlib import Path
import argparse
import base64
import io
import struct
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
CONFIG = {'app': 'tivi', 'assets': 'assets', 'stem': 'tivi-icon', 'ico': 'assets/tivi.ico', 'master': 'assets/icon-master.png', 'pattern': 'icon-{size}.png', 'header': 'src/icon.h'}
SIZES = (16, 24, 32, 48, 64, 128, 256)


def png_bytes(image):
    stream = io.BytesIO()
    image.save(stream, format='PNG')
    return stream.getvalue()


def c_array(name, data):
    lines = [f'static const unsigned char {name}[] = {{']
    lines += ['    ' + ','.join(str(b) for b in data[i:i+24]) + ',' for i in range(0, len(data), 24)]
    return '\n'.join(lines + ['};\n'])


def pack_rgba(image):
    # PackBits over whole RGBA pixels: high bit = repeat; low 7 bits = count-1.
    raw = image.tobytes()
    pixels = [raw[i:i+4] for i in range(0, len(raw), 4)]
    out = bytearray()
    i = 0
    while i < len(pixels):
        end = i + 1
        while end < min(i+128, len(pixels)) and pixels[end] == pixels[i]:
            end += 1
        if end-i >= 3:
            out.append(128 | (end-i-1))
            out.extend(pixels[i])
        else:
            end = i
            while end < min(i+128, len(pixels)):
                if end+2 < len(pixels) and pixels[end] == pixels[end+1] == pixels[end+2]:
                    break
                end += 1
            out.append(end-i-1)
            out.extend(b''.join(pixels[i:end]))
        i = end
    return out


def runtime_header(frames):
    parts = ['/* Generated from icon-master.png. Do not edit; run the icon exporter. */',
             '#ifndef APP_ICON_RASTER_H\n#define APP_ICON_RASTER_H',
             '#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>']
    for size in SIZES:
        parts.append(c_array(f'app_icon_{size}', pack_rgba(frames[size])))
    parts.append('''static void icon_render_rgba(uint8_t *rgba, int size) {
    if (!rgba || size < 1 || size > 4096) return;
    const unsigned char *data = app_icon_256;
    int source_size = 256;
    switch (size) {''')
    for size in SIZES:
        parts.append(f'    case {size}: data = app_icon_{size}; source_size = {size}; break;')
    parts.append('''    }
    size_t count = (size_t)source_size * source_size;
    uint8_t *pixels = size == source_size ? rgba : (uint8_t *)malloc(count * 4);
    if (!pixels) { memset(rgba, 0, (size_t)size * size * 4); return; }
    size_t pos = 0;
    while (pos < count) {
        unsigned token = *data++;
        size_t run = (token & 127) + 1;
        if (token & 128) {
            for (size_t i = 0; i < run; ++i) memcpy(pixels + (pos+i)*4, data, 4);
            data += 4;
        } else {
            memcpy(pixels + pos*4, data, run*4);
            data += run*4;
        }
        pos += run;
    }
    if (pixels != rgba) {
        /* Compatibility for unusual sizes. Bundle artwork uses full-resolution PNGs. */
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x)
            memcpy(rgba + ((size_t)y*size+x)*4,
                   pixels + ((size_t)(y*source_size/size)*source_size+x*source_size/size)*4, 4);
        free(pixels);
    }
}
#endif
''')
    return '\n'.join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / CONFIG['master'])
    parser.add_argument('--out', type=Path, default=ROOT / CONFIG['ico'])
    parser.add_argument('--assets-dir', type=Path, default=ROOT / CONFIG['assets'])
    parser.add_argument('--png-sizes', type=int, nargs='+', default=list(SIZES) + [512, 1024])
    args = parser.parse_args()
    if any(n < 1 or n > 1024 for n in args.png_sizes):
        parser.error('PNG sizes must be between 1 and 1024.')
    with Image.open(args.source) as image:
        if image.width != image.height:
            parser.error('The master must be square.')
        master = image.convert('RGBA')
    if master.getchannel('A').getextrema()[0] != 0:
        parser.error('The master must retain a transparent background.')
    args.assets_dir.mkdir(parents=True, exist_ok=True)
    frames = {n: master.resize((n,n), Image.Resampling.LANCZOS)
              for n in sorted(set(SIZES + (512,1024) + tuple(args.png_sizes)))}
    pngs = {n: png_bytes(frame) for n, frame in frames.items()}
    for n in args.png_sizes:
        (args.assets_dir / CONFIG['pattern'].format(size=n)).write_bytes(pngs[n])
    (args.assets_dir / (CONFIG['stem'] + '.png')).write_bytes(pngs[1024])
    encoded = base64.b64encode(pngs[1024]).decode('ascii')
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">\n'
           f'<title>{CONFIG["app"]} application icon</title>\n'
           f'<image width="1024" height="1024" href="data:image/png;base64,{encoded}"/>\n</svg>\n')
    (args.assets_dir / (CONFIG['stem'] + '.svg')).write_text(svg, encoding='utf8')
    offset = 6 + 16 * len(SIZES)
    directory = bytearray(struct.pack('<HHH', 0, 1, len(SIZES)))
    for n in SIZES:
        directory.extend(struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(pngs[n]), offset))
        offset += len(pngs[n])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(directory + b''.join(pngs[n] for n in SIZES))
    if CONFIG.get('header'):
        (ROOT / CONFIG['header']).write_text(runtime_header(frames), encoding='utf8')
    if CONFIG.get('png_header'):
        text = '/* Generated from the approved master by tools/make-icon.py. */\n#ifndef TARMAN_LOGO_PNG_H\n#define TARMAN_LOGO_PNG_H\n'
        text += '\n'.join(c_array(f'LOGO_PNG_{n}', pngs[n]) for n in (32,64,256))
        (ROOT / CONFIG['png_header']).write_text(text + '\n#endif\n', encoding='utf8')
    print(f'{CONFIG["app"]}: exported transparent PNGs, SVG wrapper, ICO and runtime assets')


if __name__ == '__main__':
    main()
