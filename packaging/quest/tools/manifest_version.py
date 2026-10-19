#!/usr/bin/env python3
"""Set versionCode and versionName in a compiled (binary) AndroidManifest.xml.

usage: manifest_version.py in.xml out.xml VERSION_CODE VERSION_NAME
A name that fits the current one's string pool entry is written in place
(padded with NULs); a longer one is appended to the string pool.
"""
import struct
import sys

from manifest_add_permission import build_pool, read_pool


def main():
    src, dst, code, name = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    d = bytearray(open(src, 'rb').read())
    sp = 8
    _, hsz, _, count, _, flags, str_start, _ = struct.unpack_from('<HHIIIIII', d, sp)
    if flags & 0x100:
        sys.exit('UTF-8 string pools are not supported')
    offs = struct.unpack_from('<%dI' % count, d, sp + hsz)

    def entry(i):
        o = sp + str_start + offs[i]
        n = struct.unpack_from('<H', d, o)[0]
        return o, n, d[o + 2:o + 2 + 2 * n].decode('utf-16le')

    strings = [entry(i)[2] for i in range(count)]
    vc, vn = strings.index('versionCode'), strings.index('versionName')
    done_code = done_name = False
    appended = False
    i = 0
    while i >= 0:
        i = d.find(struct.pack('<I', vc), i)
        if i >= 0 and d[i + 8:i + 12] == b'\x08\x00\x00\x10':
            struct.pack_into('<I', d, i + 12, code)
            done_code = True
        if i >= 0:
            i += 1
    i = 0
    while i >= 0:
        i = d.find(struct.pack('<I', vn), i)
        if i >= 0 and d[i + 8:i + 12] == b'\x08\x00\x00\x03':
            raw = struct.unpack_from('<I', d, i + 4)[0]
            o, n, old = entry(raw)
            if len(name) <= n:
                d[o:o + 2] = struct.pack('<H', len(name))
                d[o + 2:o + 2 + 2 * n] = name.encode('utf-16le') + b'\0\0' * (n - len(name))
            else:
                # Point the attribute (raw value and typed data) at a new
                # string, appended to the pool below.
                struct.pack_into('<I', d, i + 4, count)
                struct.pack_into('<I', d, i + 12, count)
                appended = True
            done_name = True
        if i >= 0:
            i += 1
    if not (done_code and done_name):
        sys.exit('versionCode/versionName attribute not found')
    if appended:
        pool = read_pool(bytes(d), sp)
        body = d[sp + pool['size']:]
        d = bytearray(d[:sp]) + build_pool(pool, strings + [name]) + body
        struct.pack_into('<I', d, 4, len(d))
    open(dst, 'wb').write(d)


if __name__ == '__main__':
    main()
