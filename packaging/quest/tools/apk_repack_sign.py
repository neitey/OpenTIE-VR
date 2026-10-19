#!/usr/bin/env python3
"""Repack an APK with replaced entries, align it and sign it (APK Signature Scheme v2).

usage: apk_repack_sign.py base.apk out.apk key.pem cert.der [archive_name=local_path ...]
"""
import hashlib
import struct
import sys
import zipfile
import zlib

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding


def build_zip(base, replacements):
    src = zipfile.ZipFile(base)
    out = bytearray()
    central = bytearray()
    count = 0
    names = [i.filename for i in src.infolist()]
    extra_new = [n for n in replacements if n not in names]
    for info in src.infolist() + [None] * len(extra_new):
        if info is None:
            name = extra_new.pop(0)
            method = zipfile.ZIP_DEFLATED
        else:
            name = info.filename
            method = info.compress_type
        if name.startswith('META-INF/'):
            continue
        data = open(replacements[name], 'rb').read() if name in replacements else src.read(name)
        if name == 'resources.arsc':
            method = zipfile.ZIP_STORED
        crc = zlib.crc32(data) & 0xffffffff
        if method == zipfile.ZIP_DEFLATED:
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            payload = c.compress(data) + c.flush()
        else:
            payload = data
        fname = name.encode()
        offset = len(out)
        extra = b''
        if method == zipfile.ZIP_STORED:
            align = 4
            data_start = offset + 30 + len(fname)
            pad = (align - data_start % align) % align
            extra = b'\0' * pad
        dostime, dosdate = 0, (1980 - 1980) << 9 | 1 << 5 | 1
        out += struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, 0x0800, method, dostime, dosdate, crc,
                           len(payload), len(data), len(fname), len(extra)) + fname + extra + payload
        central += struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 20, 20, 0x0800, method, dostime, dosdate, crc,
                               len(payload), len(data), len(fname), 0, 0, 0, 0, 0, offset) + fname
        count += 1
    eocd = struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, count, count, len(central), len(out), 0)
    return bytes(out), bytes(central), eocd


def chunk_digest(sections):
    digests = []
    for sec in sections:
        for i in range(0, len(sec), 1 << 20):
            chunk = sec[i:i + (1 << 20)]
            digests.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(chunk)) + chunk).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(digests)) + b''.join(digests)).digest()


def lp(b):
    return struct.pack('<I', len(b)) + b


def sign(entries, central, eocd, key_pem, cert_der):
    key = serialization.load_pem_private_key(open(key_pem, 'rb').read(), password=None)
    cert = open(cert_der, 'rb').read()
    pub = key.public_key().public_bytes(serialization.Encoding.DER,
                                        serialization.PublicFormat.SubjectPublicKeyInfo)
    alg = 0x0103  # RSASSA-PKCS1-v1_5 with SHA2-256
    # EOCD with the central directory offset pointing at the signing block
    # (it starts where the central directory started).
    eocd_digest = eocd[:16] + struct.pack('<I', len(entries)) + eocd[20:]
    digest = chunk_digest([entries, central, eocd_digest])
    digests = lp(struct.pack('<I', alg) + lp(digest))
    certs = lp(cert)
    signed_data = lp(digests) + lp(certs) + lp(b'')
    signature = key.sign(signed_data, padding.PKCS1v15(), hashes.SHA256())
    signer = lp(signed_data) + lp(lp(struct.pack('<I', alg) + lp(signature))) + lp(pub)
    v2_value = lp(lp(signer))
    pair = struct.pack('<Q', 4 + len(v2_value)) + struct.pack('<I', 0x7109871a) + v2_value
    size = len(pair) + 8 + 16
    block = struct.pack('<Q', size) + pair + struct.pack('<Q', size) + b'APK Sig Block 42'
    new_cd_offset = len(entries) + len(block)
    new_eocd = eocd[:16] + struct.pack('<I', new_cd_offset) + eocd[20:]
    return entries + block + central + new_eocd


def main():
    base, out, key_pem, cert_der = sys.argv[1:5]
    repl = dict(a.split('=', 1) for a in sys.argv[5:])
    entries, central, eocd = build_zip(base, repl)
    open(out, 'wb').write(sign(entries, central, eocd, key_pem, cert_der))


if __name__ == '__main__':
    main()
