#!/usr/bin/env python3
"""Build a floppy image, and put things in it.

    mkfloppy.py <output> [--size-bytes N] [--put OFFSET FILE]...

The image is zeroed and then the pieces named with --put are copied into
it at the byte offsets given. Offsets are linear byte offsets into the
image, not cylinder/head/sector triples: the conversion from a CHS address
to an offset is exactly what the emulator's INT 13h does and is not worth
doing a second time in a different language, where the two could disagree.

    # an empty 1.44 MB floppy
    mkfloppy.py disk.img

    # the same, with a boot sector and some data
    mkfloppy.py disk.img --put 0 boot.bin --put 0x200 data.bin

Offsets accept decimal or any of Python's own prefixes (0x200), because
the places a file goes are quoted in hex everywhere else in this tree.

Why this exists: a test that wants a sector to read has to have put
something there first, and building a 1 474 560-byte array by hand in C
is slow to write, slow to read, and says nothing about the shape of the
image. This script is the one place that knows what an image looks like,
so a change to the geometry changes one file and not every test.

A --put that does not fit is an error rather than a truncation. An image
quietly shorter than it should be is the kind of mistake that shows up
much later, as a sector that reads back zero -- which is also what an
untouched sector looks like, so there is nothing to notice.

The output is a plain file of exactly --size-bytes bytes, so whatever
reads it later does not have to know anything about this script.
"""

import argparse
import sys

# A 1.44 MB floppy: 80 cylinders x 2 heads x 18 sectors x 512 bytes. The
# same number as BIOS13_FLOPPY_BYTES in dos/bios/bios13.h, which is where
# the emulator's copy of this fact lives.
FLOPPY_BYTES = 80 * 2 * 18 * 512


def build_image(size, pieces, read):
    """The image, or an error string if a piece does not fit."""
    image = bytearray(size)

    for offset, path in pieces:
        data = read(path)

        if offset < 0:
            return None, "offset %d is before the start of the image" % offset

        if offset + len(data) > size:
            return None, (
                "%s is %d bytes at offset %d, which ends at %d -- past the "
                "end of a %d-byte image"
                % (path, len(data), offset, offset + len(data), size))

        image[offset:offset + len(data)] = data

    return image, None


def main(argv):
    parser = argparse.ArgumentParser(
        description="Build a floppy image, and put things in it.")

    parser.add_argument("output", help="the image to write")
    parser.add_argument("--size-bytes", type=int, default=FLOPPY_BYTES,
                        metavar="N",
                        help="how big the image is (default: %d, a 1.44 MB "
                             "floppy)" % FLOPPY_BYTES)
    parser.add_argument("--put", nargs=2, action="append", default=[],
                        metavar=("OFFSET", "FILE"),
                        help="copy FILE into the image at OFFSET; may be "
                             "given more than once")

    args = parser.parse_args(argv)

    if args.size_bytes < 0:
        print("mkfloppy.py: --size-bytes cannot be negative", file=sys.stderr)
        return 2

    # Parsed here rather than by argparse so that the prefix rule can be
    # int(x, 0) -- 0x200 as well as 512 -- and so that a bad offset names
    # itself instead of being reported as a positional argument.
    pieces = []
    for offset_text, path in args.put:
        try:
            offset = int(offset_text, 0)
        except ValueError:
            print("mkfloppy.py: --put offset must be a number, got %r"
                  % offset_text, file=sys.stderr)
            return 2

        pieces.append((offset, path))

    def read(path):
        with open(path, "rb") as handle:
            return handle.read()

    try:
        image, error = build_image(args.size_bytes, pieces, read)
    except OSError as problem:
        print("mkfloppy.py: %s" % problem, file=sys.stderr)
        return 1

    if error is not None:
        print("mkfloppy.py: %s" % error, file=sys.stderr)
        return 2

    with open(args.output, "wb") as handle:
        handle.write(image)

    filled = ", ".join("%s at %d" % (path, offset)
                       for offset, path in pieces)
    print("mkfloppy.py: wrote %d bytes to %s%s"
          % (len(image), args.output, (" with " + filled) if filled else ""))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
