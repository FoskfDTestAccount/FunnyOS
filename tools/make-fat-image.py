#!/usr/bin/env python3
"""Deterministic, independently readable DOS 360 KiB FAT12 fixture.
No timestamp from the host enters the image. Optional NAME=path arguments
allow embedding other 8.3 .COM files for the generic VM runner.
"""
import pathlib
import struct
import sys


def build(files):
    image = bytearray(720 * 512)
    image[:11] = b'\xeb\x3c\x90FUNNYOS '
    struct.pack_into('<HBHBHHBHHHII', image, 11,
                     512, 2, 1, 2, 112, 720, 0xFD, 2, 9, 2, 0, 0)
    image[38] = 0x29
    struct.pack_into('<I', image, 39, 0x46554E59)
    image[43:54] = b'FUNNYOS    '
    image[54:62] = b'FAT12   '
    image[510:512] = b'\x55\xaa'
    fat = bytearray(1024)
    fat[:3] = b'\xfd\xff\xff'

    def set_cluster(c, value):
        offset = c + c // 2
        old = struct.unpack_from('<H', fat, offset)[0]
        new = (old & 0xF) | (value << 4) if c & 1 else (old & 0xF000) | value
        struct.pack_into('<H', fat, offset, new)

    cluster = 2
    for index, (name, data) in enumerate(files):
        if index >= 112:
            raise ValueError('root directory full')
        stem, _, ext = name.upper().partition('.')
        if not 1 <= len(stem) <= 8 or len(ext) > 3 or '.' in ext:
            raise ValueError('not an 8.3 name: ' + name)
        entry = 5 * 512 + index * 32
        image[entry:entry+11] = (stem.ljust(8) + ext.ljust(3)).encode('ascii')
        image[entry+11] = 0x20
        struct.pack_into('<HHHI', image, entry+22, 0, 0x5D41,
                         cluster if data else 0, len(data))
        count = (len(data)+1023)//1024
        if cluster+count > 356:
            raise ValueError('volume full')
        for i in range(count):
            c = cluster+i
            set_cluster(c, c+1 if i+1 < count else 0xFFF)
            offset = 12*512+(c-2)*1024
            chunk = data[i*1024:(i+1)*1024]
            image[offset:offset+len(chunk)] = chunk
        cluster += count
    image[512:1536] = fat
    image[1536:2560] = fat
    return image


def main():
    if len(sys.argv) < 2:
        raise SystemExit('usage: make-fat-image.py OUTPUT [NAME=PATH ...]')
    files = [('HELLO.TXT', b'W5 FAT says hello\r\n'),
             ('EMPTY.TXT', b''),
             ('BIG.BIN', bytes(i % 251 for i in range(4097)))]
    for arg in sys.argv[2:]:
        name, path = arg.split('=', 1)
        files.append((name, pathlib.Path(path).read_bytes()))
    pathlib.Path(sys.argv[1]).write_bytes(build(files))


if __name__ == '__main__':
    main()
