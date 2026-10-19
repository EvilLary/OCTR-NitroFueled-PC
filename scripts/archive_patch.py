"""Archive patches: a game archive as the game should read it, the archive file itself unchanged.
The runtime (src/runtime_file.c, BB_ARCHIVE_PATCHES) applies one when the game opens the archive
named in it, and only while the archive still has the size it was made for.

    "BBPAK001", u64 archive size, u32 path length, guest path (/app0/archives/...),
    u32 count, then count x {u64 offset, u32 length, bytes}

A range below the archive's size replaces those bytes; ranges at or after it are added after its
end (anything between is zeros), and the game sees the archive that much longer.
"""
import os
import struct
from pathlib import Path

MAGIC = b'BBPAK001'
SECTOR = 0x800


def write(target, archive, guest, ranges):
    size = Path(archive).stat().st_size
    path = guest.encode('utf-8')
    out = bytearray(MAGIC + struct.pack('<QI', size, len(path)) + path + struct.pack('<I', len(ranges)))
    for offset, data in sorted(ranges):
        out += struct.pack('<QI', offset, len(data)) + data
    target = Path(target)
    temp = target.with_suffix('.tmp')
    temp.write_bytes(out)
    os.replace(temp, target)


def read(path):
    """(archive size, guest path, [(offset, bytes)])."""
    blob = Path(path).read_bytes()
    if blob[:8] != MAGIC:
        raise ValueError('not an archive patch')
    size, n = struct.unpack_from('<QI', blob, 8)
    at = 20
    guest = blob[at:at + n].decode('utf-8')
    at += n
    count, = struct.unpack_from('<I', blob, at)
    at += 4
    ranges = []
    for _ in range(count):
        offset, length = struct.unpack_from('<QI', blob, at)
        at += 12
        ranges.append((offset, blob[at:at + length]))
        at += length
    return size, guest, ranges


def write_entry_patch(target, archive, guest, index, payload):
    """A patch that replaces entry index of an IGA v13 archive with payload, stored uncompressed
    after the end of the archive (on a sector boundary)."""
    size = Path(archive).stat().st_size
    with open(archive, 'rb') as f:
        header = f.read(0x38)
        count, = struct.unpack_from('<I', header, 0xc)
        if index >= count:
            raise ValueError('no such entry')
        record_at = 0x38 + 4 * count + 16 * index
        f.seek(record_at)
        _offset, extra, _length, _mode = struct.unpack('<4I', f.read(16))
    position = (size + SECTOR - 1) // SECTOR * SECTOR
    record = struct.pack('<4I', position & 0xFFFFFFFF, (extra & 0xFFFFFF00) | (position >> 32),
                         len(payload), 0xFFFFFFFF)
    write(target, archive, guest, [(record_at, record), (position, payload)])


def view(archive, patch, offset, length):
    """What the game reads (for tests): the archive with the patch applied."""
    size, _guest, ranges = read(patch)
    end_virtual = max([size] + [o + len(d) for o, d in ranges])
    length = max(0, min(length, end_virtual - offset))
    out = bytearray(length)
    with open(archive, 'rb') as f:
        if offset < size:
            f.seek(offset)
            got = f.read(min(length, size - offset))
            out[:len(got)] = got
    for o, d in ranges:
        lo, hi = max(o, offset), min(o + len(d), offset + length)
        if lo < hi:
            out[lo - offset:hi - offset] = d[lo - o:hi - o]
    return bytes(out)
