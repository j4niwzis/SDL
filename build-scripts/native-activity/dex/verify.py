#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Check the generated DEX header and its native method ABI against the JNI bridge."""
import hashlib
from pathlib import Path
import re
import struct
import sys
import zlib


def verify(path):
    data = path.read_bytes()
    def u32(offset):
        return struct.unpack_from('<I', data, offset)[0]
    def u16(offset):
        return struct.unpack_from('<H', data, offset)[0]
    def uleb(offset):
        result = shift = 0
        while True:
            byte = data[offset]
            offset += 1
            result |= (byte & 127) << shift
            if byte < 128:
                return result, offset
            shift += 7
            assert shift < 35, 'invalid ULEB128'

    assert data[:8] == b'dex\n035\0', 'unexpected DEX version'
    assert u32(32) == len(data) and u32(36) == 112, 'invalid header size'
    assert u32(40) == 0x12345678, 'unexpected endian tag'
    assert u32(8) == zlib.adler32(data[12:]), 'checksum mismatch'
    assert data[12:32] == hashlib.sha1(data[32:]).digest(), 'signature mismatch'
    for offset in range(56, 104, 8):
        count, start = u32(offset), u32(offset + 4)
        assert (count == 0) == (start == 0), 'empty ID tables must have a zero offset'
        assert start < len(data), 'ID table outside file'

    strings = []
    for i in range(u32(56)):
        _, start = uleb(u32(u32(60) + i * 4))
        # Every bridge descriptor/name is ASCII, also valid modified UTF-8.
        strings.append(data[start:data.index(0, start)].decode('ascii'))
    types = [strings[u32(u32(68) + i * 4)] for i in range(u32(64))]
    prototypes = []
    for i in range(u32(72)):
        offset = u32(76) + 12 * i
        params = u32(offset + 8)
        arguments = ''.join(types[u16(params + 4 + j * 2)] for j in range(u32(params))) if params else ''
        prototypes.append('(' + arguments + ')' + types[u32(offset + 4)])
    methods = []
    for i in range(u32(88)):
        offset = u32(92) + 8 * i
        methods.append((types[u16(offset)], strings[u32(offset + 4)], prototypes[u16(offset + 2)]))
    native = set()
    for i in range(u32(96)):
        offset = u32(100) + 32 * i
        at = u32(offset + 24)
        counts = []
        for _ in range(4):
            value, at = uleb(at)
            counts.append(value)
        for _ in range(counts[0] + counts[1]):
            _, at = uleb(at)
            _, at = uleb(at)
        for count in counts[2:]:
            index = 0
            for _ in range(count):
                delta, at = uleb(at)
                index += delta
                access, at = uleb(at)
                code, at = uleb(at)
                if access & 0x100:
                    assert code == 0, 'native methods cannot contain bytecode'
                    native.add(methods[index])
                else:
                    assert code != 0, 'constructors must contain bytecode'
    bridge = (Path(__file__).resolve().parents[3] / 'src/core/android/native/input.cpp').read_text()
    registered = set()
    for name, cls in (('activity', 'NativeActivity'), ('view', 'InputView'),
                      ('connection', 'InputConnection'), ('runner', 'Runner')):
        body = re.search(r'const JNINativeMethod ' + name + r'_methods\[\] = \{(.*?)\};', bridge, re.S).group(1)
        for method, signature in re.findall(r'NATIVE\("([^"]+)", "([^"]+)"', body):
            registered.add(('Lorg/libsdl/nativeapp/' + cls + ';', method, signature))
    assert native == registered, f'DEX/JNI mismatch: {native ^ registered}'
    assert u32(96) == 4, 'expected four callback classes'
    print(f'{path}: checksums, empty tables, four classes, {len(native)} JNI signatures verified')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: verify.py classes.dex')
    verify(Path(sys.argv[1]))
