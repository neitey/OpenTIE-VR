#!/usr/bin/env python3
"""Write classes2.dex holding org.openxwa.quest.SetupProvider, assembled by hand.

usage: dexgen_setup_provider.py out.dex

The local repack build has no Android SDK (no d8), so this writes the dex
for java/org/openxwa/quest/SetupProvider.java directly: the same methods,
instruction for instruction. ART loads classes2.dex next to the APK's
classes.dex by itself. The SDK build (build_apk.py) compiles the Java file
instead; keep the two in step.
"""
import hashlib
import struct
import sys
import zlib

CLS = 'Lorg/openxwa/quest/SetupProvider;'
SUPER = 'Landroid/content/ContentProvider;'
STR = 'Ljava/lang/String;'
FILE = 'Ljava/io/File;'
BUNDLE = 'Landroid/os/Bundle;'
URI = 'Landroid/net/Uri;'
CV = 'Landroid/content/ContentValues;'
PFD = 'Landroid/os/ParcelFileDescriptor;'
ZIS = 'Ljava/util/zip/ZipInputStream;'
ZE = 'Ljava/util/zip/ZipEntry;'
FIS = 'Ljava/io/FileInputStream;'
FOS = 'Ljava/io/FileOutputStream;'
BIS = 'Ljava/io/BufferedInputStream;'
IS = 'Ljava/io/InputStream;'

ACC_PUBLIC, ACC_PRIVATE, ACC_STATIC, ACC_CONSTRUCTOR = 0x1, 0x2, 0x8, 0x10000

# ---- method references: (class, name, return, (params...)) ----
M = {
    'super_init': (SUPER, '<init>', 'V', ()),
    'getContext': (SUPER, 'getContext', 'Landroid/content/Context;', ()),
    'getExtDir': ('Landroid/content/Context;', 'getExternalFilesDir', FILE, (STR,)),
    'getObbDir': ('Landroid/content/Context;', 'getObbDir', FILE, ()),
    'callingUid': ('Landroid/os/Binder;', 'getCallingUid', 'I', ()),
    'bundle_init': (BUNDLE, '<init>', 'V', ()),
    'putInt': (BUNDLE, 'putInt', 'V', (STR, 'I')),
    'putString': (BUNDLE, 'putString', 'V', (STR, STR)),
    'pfd_open': (PFD, 'open', PFD, (FILE, 'I')),
    'pfd_parseMode': (PFD, 'parseMode', 'I', (STR,)),
    'file_init': (FILE, '<init>', 'V', (FILE, STR)),
    'canon': (FILE, 'getCanonicalPath', STR, ()),
    'isDir': (FILE, 'isDirectory', 'Z', ()),
    'mkdirs': (FILE, 'mkdirs', 'Z', ()),
    'fdelete': (FILE, 'delete', 'Z', ()),
    'exists': (FILE, 'exists', 'Z', ()),
    'getPath': (FILE, 'getPath', STR, ()),
    'parent': (FILE, 'getParentFile', FILE, ()),
    'concat': (STR, 'concat', STR, (STR,)),
    'split': (STR, 'split', '[Ljava/lang/String;', (STR,)),
    'startsWith': (STR, 'startsWith', 'Z', (STR,)),
    'equals': (STR, 'equals', 'Z', ('Ljava/lang/Object;',)),
    'toString': ('Ljava/lang/Throwable;', 'toString', STR, ()),
    'fis_init': (FIS, '<init>', 'V', (FILE,)),
    'bis_init': (BIS, '<init>', 'V', (IS, 'I')),
    'zis_init': (ZIS, '<init>', 'V', (IS,)),
    'zis_next': (ZIS, 'getNextEntry', ZE, ()),
    'zis_read': (ZIS, 'read', 'I', ('[B', 'I', 'I')),
    'zis_close': (ZIS, 'close', 'V', ()),
    'ze_name': (ZE, 'getName', STR, ()),
    'ze_isDir': (ZE, 'isDirectory', 'Z', ()),
    'fos_init': (FOS, '<init>', 'V', (FILE,)),
    'fos_write': (FOS, 'write', 'V', ('[B', 'I', 'I')),
    'fos_close': (FOS, 'close', 'V', ()),
    # this class
    'init': (CLS, '<init>', 'V', ()),
    'onCreate': (CLS, 'onCreate', 'Z', ()),
    'openFile': (CLS, 'openFile', PFD, (URI, STR)),
    'call': (CLS, 'call', BUNDLE, (STR, STR, BUNDLE)),
    'run': (CLS, 'run', BUNDLE, (STR, STR)),
    'mk': (CLS, 'mkdirs', BUNDLE, (FILE, STR, STR)),
    'unzip': (CLS, 'unzip', BUNDLE, (FILE, STR, STR)),
    'query': (CLS, 'query', 'Landroid/database/Cursor;', (URI, '[Ljava/lang/String;', STR, '[Ljava/lang/String;', STR)),
    'getType': (CLS, 'getType', STR, (URI,)),
    'insert': (CLS, 'insert', URI, (URI, CV)),
    'delete': (CLS, 'delete', 'I', (URI, STR, '[Ljava/lang/String;')),
    'update': (CLS, 'update', 'I', (URI, CV, STR, '[Ljava/lang/String;')),
}


# ---- a tiny assembler: ('op', args...) and ('label', name) ----
def size_of(ins):
    op = ins[0]
    if op == 'label':
        return 0
    if op in ('invoke-virtual', 'invoke-direct', 'invoke-static'):
        return 3
    if op in ('const-string', 'new-instance', 'new-array', 'const/16', 'const/high16', 'if-eq', 'if-ge',
              'if-eqz', 'if-nez', 'if-lez', 'goto/16', 'aget-object', 'add-int/lit8'):
        return 2
    return 1


INVOKE = {'invoke-virtual': 0x6e, 'invoke-direct': 0x70, 'invoke-static': 0x71}
OP11X = {'move-result': 0x0a, 'move-result-object': 0x0c, 'move-exception': 0x0d, 'return': 0x0f,
         'return-object': 0x11}
OP21T = {'if-eqz': 0x38, 'if-nez': 0x39, 'if-lez': 0x3d}
OP22T = {'if-eq': 0x32, 'if-ge': 0x35}


def assemble(code, idx):
    """code -> (list of 16-bit units, label addresses). idx resolves refs."""
    labels, pc = {}, 0
    for ins in code:
        if ins[0] == 'label':
            labels[ins[1]] = pc
        pc += size_of(ins)
    out = []
    pc = 0
    for ins in code:
        op, a = ins[0], ins[1:]
        if op == 'label':
            continue
        if op in INVOKE:
            regs, m = a
            assert len(regs) <= 4 and all(r < 16 for r in regs)
            r = list(regs) + [0] * (4 - len(regs))
            out += [(len(regs) << 12) | INVOKE[op], idx['method'][M[m]],
                    (r[3] << 12) | (r[2] << 8) | (r[1] << 4) | r[0]]
        elif op in OP11X:
            out += [(a[0] << 8) | OP11X[op]]
        elif op == 'return-void':
            out += [0x000e]
        elif op == 'const/4':
            assert -8 <= a[1] < 8 and a[0] < 16
            out += [((a[1] & 0xf) << 12) | (a[0] << 8) | 0x12]
        elif op == 'const/16':
            out += [(a[0] << 8) | 0x13, a[1] & 0xffff]
        elif op == 'const/high16':
            out += [(a[0] << 8) | 0x15, a[1] & 0xffff]
        elif op == 'const-string':
            out += [(a[0] << 8) | 0x1a, idx['string'][a[1]]]
        elif op == 'new-instance':
            out += [(a[0] << 8) | 0x22, idx['type'][a[1]]]
        elif op == 'new-array':
            assert a[0] < 16 and a[1] < 16
            out += [(a[1] << 12) | (a[0] << 8) | 0x23, idx['type'][a[2]]]
        elif op == 'array-length':
            out += [(a[1] << 12) | (a[0] << 8) | 0x21]
        elif op == 'aget-object':
            out += [(a[0] << 8) | 0x46, (a[2] << 8) | a[1]]
        elif op == 'add-int/lit8':
            out += [(a[0] << 8) | 0xd8, ((a[2] & 0xff) << 8) | a[1]]
        elif op in OP21T:
            out += [(a[0] << 8) | OP21T[op], (labels[a[1]] - pc) & 0xffff]
        elif op in OP22T:
            assert a[0] < 16 and a[1] < 16
            out += [(a[1] << 12) | (a[0] << 8) | OP22T[op], (labels[a[2]] - pc) & 0xffff]
        elif op == 'goto/16':
            out += [0x0029, (labels[a[0]] - pc) & 0xffff]
        else:
            raise ValueError(op)
        pc += size_of(ins)
        assert len(out) == pc, ins
    return out, labels


# ---- the methods (registers, ins, outs, code, try) ----
def uid_check(lbl):
    return [('invoke-static', (), 'callingUid'), ('move-result', 0), ('const/16', 1, 2000),
            ('if-eq', 0, 1, lbl), ('if-eqz', 0, lbl), ('const/4', 0, 0), ('return-object', 0), ('label', lbl)]


METHODS = {}
METHODS['init'] = (1, 1, 1, [('invoke-direct', (0,), 'super_init'), ('return-void',)], None)
METHODS['onCreate'] = (2, 1, 0, [('const/4', 0, 1), ('return', 0)], None)
for name, nargs in (('query', 5), ('getType', 1), ('insert', 2)):
    METHODS[name] = (1 + 1 + nargs, 1 + nargs, 0, [('const/4', 0, 0), ('return-object', 0)], None)
for name, nargs in (('delete', 3), ('update', 4)):
    METHODS[name] = (1 + 1 + nargs, 1 + nargs, 0, [('const/4', 0, 0), ('return', 0)], None)

# openFile(uri, mode): R=6, p0=v3 p1=v4 p2=v5
METHODS['openFile'] = (6, 3, 3, uid_check('ok') + [
    ('invoke-virtual', (3,), 'getContext'), ('move-result-object', 0),
    ('const/4', 1, 0),
    ('invoke-virtual', (0, 1), 'getExtDir'), ('move-result-object', 0),
    ('new-instance', 1, FILE),
    ('const-string', 2, '.openxwa-import.zip'),
    ('invoke-direct', (1, 0, 2), 'file_init'),
    ('invoke-virtual', (1,), 'fdelete'),
    ('invoke-static', (5,), 'pfd_parseMode'), ('move-result', 0),
    ('invoke-static', (1, 0), 'pfd_open'), ('move-result-object', 0),
    ('return-object', 0),
], None)

# call(method, arg, extras): R=7, p0=v3 p1=v4 p2=v5 p3=v6; try around run()
METHODS['call'] = (7, 4, 3, uid_check('ok') + [
    ('label', 'try_start'),
    ('invoke-direct', (3, 4, 5), 'run'), ('move-result-object', 0),
    ('label', 'try_end'),
    ('return-object', 0),
    ('label', 'handler'),
    ('move-exception', 0),
    ('new-instance', 1, BUNDLE),
    ('invoke-direct', (1,), 'bundle_init'),
    ('const-string', 2, 'error'),
    ('invoke-virtual', (0,), 'toString'), ('move-result-object', 0),
    ('invoke-virtual', (1, 2, 0), 'putString'),
    ('return-object', 1),
], ('try_start', 'try_end', 'handler'))

# run(method, arg): R=6, p0=v3 p1=v4 p2=v5
METHODS['run'] = (6, 3, 4, [
    ('invoke-virtual', (3,), 'getContext'), ('move-result-object', 0),
    ('const/4', 1, 0),
    ('invoke-virtual', (0, 1), 'getExtDir'), ('move-result-object', 0),
    ('if-nez', 0, 'have_base'),
    ('return-object', 1),
    ('label', 'have_base'),
    ('invoke-virtual', (0,), 'canon'), ('move-result-object', 1),
    ('const-string', 2, '/'),
    ('invoke-virtual', (1, 2), 'concat'), ('move-result-object', 1),
    ('invoke-virtual', (3,), 'getContext'), ('move-result-object', 2),
    ('invoke-virtual', (2,), 'getObbDir'),
    ('const-string', 2, 'unzip'),
    ('invoke-virtual', (2, 4), 'equals'), ('move-result', 2),
    ('if-eqz', 2, 'mk'),
    ('invoke-direct', (3, 0, 1, 5), 'unzip'), ('move-result-object', 0),
    ('return-object', 0),
    ('label', 'mk'),
    ('invoke-direct', (3, 0, 1, 5), 'mk'), ('move-result-object', 0),
    ('return-object', 0),
], None)

# mkdirs(base, prefix, arg): R=11, p0=v7 p1=v8 p2=v9 p3=v10
METHODS['mk'] = (11, 4, 3, [
    ('if-nez', 10, 'have'),
    ('const-string', 10, 'XWA'),
    ('label', 'have'),
    ('const-string', 0, ':'),
    ('invoke-virtual', (10, 0), 'split'), ('move-result-object', 0),
    ('array-length', 1, 0),
    ('const/4', 2, 0),
    ('const/4', 3, 0),
    ('label', 'loop'),
    ('if-ge', 2, 1, 'done'),
    ('aget-object', 4, 0, 2),
    ('new-instance', 5, FILE),
    ('invoke-direct', (5, 8, 4), 'file_init'),
    ('invoke-virtual', (5,), 'canon'), ('move-result-object', 6),
    ('invoke-virtual', (6, 9), 'startsWith'), ('move-result', 6),
    ('if-eqz', 6, 'next'),
    ('invoke-virtual', (5,), 'mkdirs'),
    ('invoke-virtual', (5,), 'isDir'), ('move-result', 6),
    ('if-eqz', 6, 'next'),
    ('add-int/lit8', 3, 3, 1),
    ('label', 'next'),
    ('add-int/lit8', 2, 2, 1),
    ('goto/16', 'loop'),
    ('label', 'done'),
    ('new-instance', 4, BUNDLE),
    ('invoke-direct', (4,), 'bundle_init'),
    ('const-string', 5, 'base'),
    ('invoke-virtual', (4, 5, 9), 'putString'),
    ('const-string', 5, 'made'),
    ('invoke-virtual', (4, 5, 3), 'putInt'),
    ('const-string', 5, 'asked'),
    ('invoke-virtual', (4, 5, 1), 'putInt'),
    ('return-object', 4),
], None)

# unzip(base, prefix, arg): R=16, p0=v12 p1=v13 p2=v14 p3=v15
# v0 dest, v1 zip, v2 ZipInputStream, v3 buf, v4 files, v5 skipped, v6 entry,
# v7 out file, v8 temp, v9 FileOutputStream, v10 n, v11 65536
METHODS['unzip'] = (16, 4, 4, [
    ('if-nez', 15, 'have'),
    ('const-string', 15, 'XWA'),
    ('label', 'have'),
    ('new-instance', 0, FILE),
    ('invoke-direct', (0, 13, 15), 'file_init'),
    ('invoke-virtual', (12,), 'getContext'), ('move-result-object', 8),
    ('invoke-virtual', (8,), 'getObbDir'), ('move-result-object', 8),
    ('new-instance', 1, FILE),
    ('const-string', 6, 'openxwa-import.zip'),
    ('invoke-direct', (1, 8, 6), 'file_init'),
    ('invoke-virtual', (1,), 'exists'), ('move-result', 8),
    ('if-nez', 8, 'have_zip'),
    ('new-instance', 1, FILE),
    ('const-string', 6, '.openxwa-import.zip'),
    ('invoke-direct', (1, 13, 6), 'file_init'),
    ('label', 'have_zip'),
    ('const/high16', 11, 0x0001),          # 65536
    ('new-instance', 6, FIS),
    ('invoke-direct', (6, 1), 'fis_init'),
    ('new-instance', 8, BIS),
    ('invoke-direct', (8, 6, 11), 'bis_init'),
    ('new-instance', 2, ZIS),
    ('invoke-direct', (2, 8), 'zis_init'),
    ('new-array', 3, 11, '[B'),
    ('const/4', 4, 0),
    ('const/4', 5, 0),
    ('label', 'loop'),
    ('invoke-virtual', (2,), 'zis_next'), ('move-result-object', 6),
    ('if-eqz', 6, 'done'),
    ('invoke-virtual', (6,), 'ze_name'), ('move-result-object', 8),
    ('new-instance', 7, FILE),
    ('invoke-direct', (7, 0, 8), 'file_init'),
    ('invoke-virtual', (7,), 'canon'), ('move-result-object', 8),
    ('invoke-virtual', (8, 14), 'startsWith'), ('move-result', 8),
    ('if-nez', 8, 'inside'),
    ('add-int/lit8', 5, 5, 1),
    ('goto/16', 'loop'),
    ('label', 'inside'),
    ('invoke-virtual', (6,), 'ze_isDir'), ('move-result', 8),
    ('if-eqz', 8, 'file'),
    ('invoke-virtual', (7,), 'mkdirs'),
    ('goto/16', 'loop'),
    ('label', 'file'),
    ('invoke-virtual', (7,), 'parent'), ('move-result-object', 8),
    ('invoke-virtual', (8,), 'mkdirs'),
    ('invoke-virtual', (7,), 'fdelete'),
    ('new-instance', 9, FOS),
    ('invoke-direct', (9, 7), 'fos_init'),
    ('label', 'copy'),
    ('const/4', 8, 0),
    ('invoke-virtual', (2, 3, 8, 11), 'zis_read'), ('move-result', 10),
    ('if-lez', 10, 'closed'),
    ('invoke-virtual', (9, 3, 8, 10), 'fos_write'),
    ('goto/16', 'copy'),
    ('label', 'closed'),
    ('invoke-virtual', (9,), 'fos_close'),
    ('add-int/lit8', 4, 4, 1),
    ('goto/16', 'loop'),
    ('label', 'done'),
    ('invoke-virtual', (2,), 'zis_close'),
    ('invoke-virtual', (1,), 'fdelete'),
    ('new-instance', 6, BUNDLE),
    ('invoke-direct', (6,), 'bundle_init'),
    ('const-string', 7, 'base'),
    ('invoke-virtual', (6, 7, 14), 'putString'),
    ('invoke-virtual', (1,), 'getPath'), ('move-result-object', 8),
    ('const-string', 7, 'zip'),
    ('invoke-virtual', (6, 7, 8), 'putString'),
    ('const-string', 7, 'files'),
    ('invoke-virtual', (6, 7, 4), 'putInt'),
    ('const-string', 7, 'skipped'),
    ('invoke-virtual', (6, 7, 5), 'putInt'),
    ('return-object', 6),
], None)

DIRECT = {'init': ACC_PUBLIC | ACC_CONSTRUCTOR, 'run': ACC_PRIVATE, 'mk': ACC_PRIVATE, 'unzip': ACC_PRIVATE}
VIRTUAL = {k: ACC_PUBLIC for k in ('onCreate', 'openFile', 'call', 'query', 'getType', 'insert', 'delete', 'update')}


def shorty_char(t):
    return 'L' if t[0] in 'L[' else t


def shorty(ret, params):
    return shorty_char(ret) + ''.join(shorty_char(p) for p in params)


def uleb(n):
    out = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def mutf8(s):
    return s.encode('utf-8')  # ASCII only here


def build():
    # Collect every string, type, proto, method.
    const_strings = set()
    for regs, ins, outs, code, tr in METHODS.values():
        for i in code:
            if i[0] == 'const-string':
                const_strings.add(i[2])
    types = {CLS, SUPER}
    type_refs = set()
    for regs, ins, outs, code, tr in METHODS.values():
        for i in code:
            if i[0] == 'new-instance':
                type_refs.add(i[2])
            if i[0] == 'new-array':
                type_refs.add(i[3])
    protos = set()
    for c, n, r, p in M.values():
        types.add(c)
        types.add(r)
        types.update(p)
        protos.add((r, p))
    types |= type_refs
    strings = set(const_strings) | types | {n for c, n, r, p in M.values()} | {shorty(r, p) for r, p in protos}

    strings = sorted(strings, key=lambda s: s.encode('utf-16-be'))
    sidx = {s: i for i, s in enumerate(strings)}
    types = sorted(types, key=lambda t: sidx[t])
    tidx = {t: i for i, t in enumerate(types)}
    protos = sorted(protos, key=lambda rp: (tidx[rp[0]], [tidx[x] for x in rp[1]]))
    pidx = {rp: i for i, rp in enumerate(protos)}
    methods = sorted(set(M.values()), key=lambda m: (tidx[m[0]], sidx[m[1]], pidx[(m[2], m[3])]))
    midx = {m: i for i, m in enumerate(methods)}
    idx = {'string': sidx, 'type': tidx, 'method': midx}

    # Layout: header, ids, class_def, then data.
    HDR = 0x70
    off = HDR
    string_ids_off = off; off += 4 * len(strings)
    type_ids_off = off; off += 4 * len(types)
    proto_ids_off = off; off += 12 * len(protos)
    method_ids_off = off; off += 8 * len(methods)
    class_defs_off = off; off += 32
    data_off = off

    data = bytearray()

    def here():
        return data_off + len(data)

    def align4():
        while here() % 4:
            data.append(0)

    # code items
    align4()
    code_start = here()
    code_off = {}
    for key, (regs, ins, outs, code, tr) in METHODS.items():
        align4()
        code_off[key] = here()
        units, labels = assemble(code, idx)
        for i in code:  # register bounds
            for r in i[1:]:
                if isinstance(r, tuple):
                    assert all(x < regs for x in r), (key, i)
        body = struct.pack('<HHHHII', regs, ins, outs, 1 if tr else 0, 0, len(units))
        body += struct.pack('<%dH' % len(units), *units)
        if tr:
            if len(units) % 2:
                body += b'\0\0'
            start, end, handler = labels[tr[0]], labels[tr[1]], labels[tr[2]]
            body += struct.pack('<IHH', start, end - start, 1)  # handler at byte 1 of the list
            body += uleb(1) + bytes([0]) + uleb(handler)  # 1 handler: size 0 (sleb), catch-all addr
        data += body
    code_count = len(METHODS)

    # type lists (parameter lists of protos)
    align4()
    tl_start = here()
    tl_off = {}
    for r, p in protos:
        if not p or p in tl_off:
            continue
        align4()
        tl_off[p] = here()
        data += struct.pack('<I', len(p)) + struct.pack('<%dH' % len(p), *[tidx[x] for x in p])
    tl_count = len(tl_off)

    # string data
    sd_start = here()
    sd_off = []
    for s in strings:
        sd_off.append(here())
        data += uleb(len(s)) + mutf8(s) + b'\0'

    # class data
    cd_off = here()
    direct = sorted(DIRECT, key=lambda k: midx[M[k]])
    virtual = sorted(VIRTUAL, key=lambda k: midx[M[k]])
    cd = uleb(0) + uleb(0) + uleb(len(direct)) + uleb(len(virtual))
    for group, flags in ((direct, DIRECT), (virtual, VIRTUAL)):
        prev = 0
        for k in group:
            mi = midx[M[k]]
            cd += uleb(mi - prev) + uleb(flags[k]) + uleb(code_off[k])
            prev = mi
    data += cd

    # map list
    align4()
    map_off = here()
    items = [
        (0x0000, 1, 0),
        (0x0001, len(strings), string_ids_off),
        (0x0002, len(types), type_ids_off),
        (0x0003, len(protos), proto_ids_off),
        (0x0005, len(methods), method_ids_off),
        (0x0006, 1, class_defs_off),
        (0x2001, code_count, code_start),
        (0x1001, tl_count, tl_start),
        (0x2002, len(strings), sd_start),
        (0x2000, 1, cd_off),
        (0x1000, 1, map_off),
    ]
    data += struct.pack('<I', len(items))
    for t, n, o in items:
        data += struct.pack('<HHII', t, 0, n, o)

    file_size = data_off + len(data)
    out = bytearray(file_size)
    hdr = struct.pack('<8sI20sIIIIIIIIIIIIIIIIIIII', b'dex\n035\0', 0, b'\0' * 20, file_size, HDR, 0x12345678,
                      0, 0, map_off, len(strings), string_ids_off, len(types), type_ids_off, len(protos),
                      proto_ids_off, 0, 0, len(methods), method_ids_off, 1, class_defs_off,
                      file_size - data_off, data_off)
    assert len(hdr) == HDR
    out[:HDR] = hdr
    o = string_ids_off
    for so in sd_off:
        struct.pack_into('<I', out, o, so); o += 4
    o = type_ids_off
    for t in types:
        struct.pack_into('<I', out, o, sidx[t]); o += 4
    o = proto_ids_off
    for r, p in protos:
        struct.pack_into('<III', out, o, sidx[shorty(r, p)], tidx[r], tl_off.get(p, 0) if p else 0); o += 12
    o = method_ids_off
    for c, n, r, p in methods:
        struct.pack_into('<HHI', out, o, tidx[c], pidx[(r, p)], sidx[n]); o += 8
    NO_INDEX = 0xffffffff
    struct.pack_into('<IIIIIIII', out, class_defs_off, tidx[CLS], ACC_PUBLIC, tidx[SUPER], 0, NO_INDEX, 0,
                     cd_off, 0)
    out[data_off:] = data
    out[12:32] = hashlib.sha1(bytes(out[32:])).digest()
    struct.pack_into('<I', out, 8, zlib.adler32(bytes(out[12:])) & 0xffffffff)
    return bytes(out)


if __name__ == '__main__':
    d = build()
    open(sys.argv[1], 'wb').write(d)
    print('%s: %d bytes' % (sys.argv[1], len(d)))
