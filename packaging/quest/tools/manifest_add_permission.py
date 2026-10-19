#!/usr/bin/env python3
"""Add a <uses-permission> to a compiled (binary) AndroidManifest.xml.

usage: manifest_add_permission.py in.xml out.xml PERMISSION [PERMISSION ...]

For repacking an existing APK without aapt2: the new elements are copies of
the manifest's first <uses-permission> with the name replaced, inserted right
after it; their names are appended to the string pool. Permissions already
present are left alone.
"""
import struct
import sys

RES_XML_START_ELEMENT = 0x0102
RES_XML_END_ELEMENT = 0x0103
RES_STRING_POOL = 0x0001
UTF8_FLAG = 0x100


def read_pool(d, off):
    typ, hsz, size, count, styles, flags, str_start, style_start = struct.unpack_from('<HHIIIIII', d, off)
    assert typ == RES_STRING_POOL
    if flags & UTF8_FLAG:
        sys.exit('UTF-8 string pools are not supported')
    offs = list(struct.unpack_from('<%dI' % count, d, off + hsz))
    strings = []
    for o in offs:
        p = off + str_start + o
        n = struct.unpack_from('<H', d, p)[0]
        if n & 0x8000:
            sys.exit('long strings are not supported')
        strings.append(d[p + 2:p + 2 + 2 * n].decode('utf-16le'))
    return dict(hsz=hsz, size=size, count=count, styles=styles, flags=flags, str_start=str_start,
                style_start=style_start, offs=offs, strings=strings)


def build_pool(pool, strings):
    data = bytearray()
    offs = []
    for s in strings:
        offs.append(len(data))
        enc = s.encode('utf-16le')
        data += struct.pack('<H', len(s)) + enc + b'\0\0'
    while len(data) % 4:
        data += b'\0'
    if pool['styles']:
        sys.exit('string pools with styles are not supported')
    hsz = pool['hsz']
    str_start = hsz + 4 * len(strings)
    size = str_start + len(data)
    head = struct.pack('<HHIIIIII', RES_STRING_POOL, hsz, size, len(strings), 0, pool['flags'], str_start, 0)
    head += b'\0' * (hsz - len(head))
    return head + struct.pack('<%dI' % len(offs), *offs) + bytes(data)


def chunks(d, start):
    off = start
    while off < len(d):
        typ, hsz, size = struct.unpack_from('<HHI', d, off)
        yield off, typ, hsz, size
        off += size


def main():
    src, dst, perms = sys.argv[1], sys.argv[2], sys.argv[3:]
    d = bytes(open(src, 'rb').read())
    _, file_hsz, _ = struct.unpack_from('<HHI', d, 0)
    pool_off = file_hsz
    pool = read_pool(d, pool_off)
    strings = list(pool['strings'])
    name_idx = strings.index('uses-permission')

    body_start = pool_off + pool['size']
    body = d[body_start:]
    # The first <uses-permission ...> start and its end element.
    first = None
    present = set()
    for off, typ, hsz, size in chunks(d, body_start):
        if typ == RES_XML_START_ELEMENT:
            el_name = struct.unpack_from('<I', d, off + 16 + 4)[0]
            if el_name == name_idx:
                attr_start, attr_size, attr_count = struct.unpack_from('<HHH', d, off + 16 + 8)
                a0 = off + 16 + attr_start
                raw = struct.unpack_from('<I', d, a0 + 8)[0]
                present.add(strings[raw])
                if first is None:
                    first = (off, size)
        elif typ == RES_XML_END_ELEMENT and first is not None and len(first) == 2:
            el_name = struct.unpack_from('<I', d, off + 16 + 4)[0]
            if el_name == name_idx:
                first = (first[0], first[1], off, size)
    if first is None or len(first) != 4:
        sys.exit('no <uses-permission> to copy')
    start_off, start_size, end_off, end_size = first
    start_el = d[start_off:start_off + start_size]
    end_el = d[end_off:end_off + end_size]

    insert = bytearray()
    for perm in perms:
        if perm in present:
            print('already present:', perm)
            continue
        idx = len(strings)
        strings.append(perm)
        el = bytearray(start_el)
        attr_start = struct.unpack_from('<H', el, 16 + 8)[0]
        a0 = 16 + attr_start
        struct.pack_into('<I', el, a0 + 8, idx)       # raw value
        struct.pack_into('<I', el, a0 + 8 + 4 + 4, idx)  # typed value data (string)
        insert += el + end_el
        print('added:', perm)

    new_pool = build_pool(pool, strings)
    after = end_off + end_size - body_start
    new_body = body[:after] + bytes(insert) + body[after:]
    out = bytearray(d[:file_hsz]) + new_pool + new_body
    struct.pack_into('<I', out, 4, len(out))
    open(dst, 'wb').write(out)


if __name__ == '__main__':
    main()
