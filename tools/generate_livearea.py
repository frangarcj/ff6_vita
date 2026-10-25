#!/usr/bin/env python3
"""Generate historical fallback art outside the packaged Standard-Republic assets."""
import argparse
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
# Small geometric lettering, defined here so no system font is a build input.
LETTERS = {
    'A': [14,17,17,31,17,17,17], 'C': [14,17,16,16,16,17,14],
    'E': [31,16,16,30,16,16,31], 'F': [31,16,16,30,16,16,16],
    'G': [14,17,16,23,17,17,14], 'I': [31,4,4,4,4,4,31],
    'L': [16,16,16,16,16,16,31], 'N': [17,25,25,21,19,19,17],
    'O': [14,17,17,17,17,17,14], 'R': [30,17,17,30,20,18,17],
    'S': [15,16,16,14,1,1,30], 'T': [31,4,4,4,4,4,4],
    'U': [17,17,17,17,17,17,14], 'V': [17,17,17,17,17,10,4],
    'Y': [17,17,10,4,4,4,4], ' ': [0]*7,
}
PALETTE = [(12+i//3, 22+i//2, 40+i) for i in range(64)] + [(231,190,109),(229,236,246),(75,118,163)]


def png(path: Path, width: int, height: int, lines: list[tuple[str, int, int]]) -> None:
    pixels = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            pixels[y*width+x] = max(0, min(63, 46-int(38*y/height)-int(12*abs(x-width/2)/width)))
    def rect(x0, y0, x1, y1, color):
        for y in range(max(0,y0), min(height,y1)):
            start, end = max(0,x0), min(width,x1)
            if end > start: pixels[y*width+start:y*width+end] = bytes([color])*(end-start)
    margin = max(5, width//32)
    rect(margin, margin, width-margin, margin+2, 64)
    rect(margin, height-margin-2, width-margin, height-margin, 66)
    for text, scale, top in lines:
        left = (width-(len(text)*6-1)*scale)//2
        for n, letter in enumerate(text):
            for y, row in enumerate(LETTERS[letter]):
                for x in range(5):
                    if row & (1 << (4-x)):
                        xx, yy = left+(n*6+x)*scale, top+y*scale
                        rect(xx, yy, xx+scale, yy+scale, 64 if text == 'VI' else 65)
    def chunk(kind, data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    rows = b''.join(b'\0'+pixels[y*width:(y+1)*width] for y in range(height))
    data = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,3,0,0,0))
    data += chunk(b'PLTE',bytes(c for color in PALETTE for c in color))
    data += chunk(b'IDAT',zlib.compress(rows,9))+chunk(b'IEND',b'')
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build_livearea_fallback',
                        help='Standalone fallback directory; never the packaged sce_sys')
    args = parser.parse_args()
    output = args.output.resolve()
    packaged = (ROOT/'sce_sys').resolve()
    if output == packaged or packaged in output.parents or output in packaged.parents:
        parser.error('Fallback artwork must not overwrite the packaged LiveArea assets')
    png(output/'icon0.png',128,128,[('VI',9,33)])
    base = output/'livearea/contents'
    png(base/'bg0.png',840,500,[('FINAL FANTASY',5,80),('VI',28,160)])
    png(base/'startup.png',280,158,[('FINAL FANTASY',2,26),('VI',10,65)])
    png(base/'config.png',256,64,[('CONFIGURE',4,18)])
    print('Generated historical fallback artwork in ' + str(output))

if __name__ == '__main__':
    main()
