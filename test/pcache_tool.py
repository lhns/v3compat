#!/usr/bin/env python3
"""The translation cache's file format (lhns/dynamorio core/pcache.c), for tests and labs.

  check FILE              structure and checksums; prints entries and bytes
  diff A B                entries for the same block that differ between two training runs
  corrupt FILE MODE OUT   a damaged copy; the modes that reseal fix the checksums up again, so
                          the reader's inner checks are what must reject them
  selftest                the hash against known XXH64 values
"""
import struct
import sys

MAGIC = 0x3265686361637076
VERSION = 2
HDR = struct.Struct('<QIIQQQIIQQQQ')       # 80
IDX = struct.Struct('<QIIQ')               # 24
ENT = struct.Struct('<QQIIQIIHHBBHII')     # 56
VAL = struct.Struct('<BBHIQ')              # 16
R_RAW, R_APPREL, R_REL, R_LABEL, R_ICTI, R_EXIT = range(1, 7)
E_PIN = 1
M = (1 << 64) - 1
P1, P2, P3, P4, P5 = (11400714785074694791, 14029467366897019727, 1609587929392839161,
                      9650029242287828579, 2870177450012600261)


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M


def _round(acc, v):
    return (_rotl((acc + v * P2) & M, 31) * P1) & M


def xxh64(b):
    n, p = len(b), 0
    if n >= 32:
        v = [(P1 + P2) & M, P2, 0, (-P1) & M]
        while p + 32 <= n:
            for i in range(4):
                v[i] = _round(v[i], int.from_bytes(b[p + 8 * i:p + 8 * i + 8], 'little'))
            p += 32
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & M
        for x in v:
            h = ((h ^ _round(0, x)) * P1 + P4) & M
    else:
        h = P5
    h = (h + n) & M
    while p + 8 <= n:
        h ^= _round(0, int.from_bytes(b[p:p + 8], 'little'))
        h = (_rotl(h, 27) * P1 + P4) & M
        p += 8
    if p + 4 <= n:
        h ^= (int.from_bytes(b[p:p + 4], 'little') * P1) & M
        h = (_rotl(h, 23) * P2 + P3) & M
        p += 4
    while p < n:
        h ^= (b[p] * P5) & M
        h = (_rotl(h, 11) * P1) & M
        p += 1
    h ^= h >> 33
    h = (h * P2) & M
    h ^= h >> 29
    h = (h * P3) & M
    h ^= h >> 32
    return h


class Cache:
    def __init__(self, data):
        self.b = bytearray(data)
        f = HDR.unpack_from(self.b, 0)
        (self.magic, self.version, self.header_size, self.file_size, self.header_hash,
         self.body_hash, self.key_len, self.num, self.key_off, self.index_off, self.data_off,
         self.data_len) = f

    def problems(self):
        b = self.b
        if self.magic != MAGIC or self.version != VERSION or self.header_size != HDR.size:
            return 'magic or version'
        if self.file_size != len(b):
            return 'size'
        h = bytearray(b[:HDR.size])
        h[24:32] = bytes(8)
        if xxh64(bytes(h)) != self.header_hash:
            return 'header hash'
        if xxh64(bytes(b[HDR.size:])) != self.body_hash:
            return 'body hash'
        for i in range(self.num):
            modid, off, ln, doff = self.index(i)
            e = self.data_off + doff
            if xxh64(bytes(b[e + 8:e + ln])) != struct.unpack_from('<Q', b, e)[0]:
                return 'entry %d hash' % i
        return None

    def index(self, i):
        return IDX.unpack_from(self.b, self.index_off + i * IDX.size)

    def entry_at(self, i):
        modid, off, ln, doff = self.index(i)
        return self.data_off + doff, ln

    def entries(self):
        for i in range(self.num):
            modid, off, ln, doff = self.index(i)
            yield (modid, off), self.data_off + doff, ln

    def records(self, e):
        """(kind, offset of the record's first byte after the kind, [offsets of its values])"""
        b = self.b
        f = ENT.unpack_from(b, e)
        src_len, nrec, rec_len = f[7], f[8], f[12]
        p = e + ENT.size + src_len
        out = []
        for _ in range(nrec):
            k = b[p]
            p += 1
            vals = []
            q = p
            if k == R_RAW:
                ln, nfix = struct.unpack_from('<HH', b, p)
                p += 4 + ln
                for j in range(nfix):
                    vals.append(p + 4)
                    p += 4 + VAL.size
            elif k == R_APPREL:
                p += 4
            elif k == R_REL:
                ln = b[p]
                vals.append(p + 2)
                p += 2 + VAL.size + ln
            elif k == R_LABEL:
                pass
            elif k == R_ICTI:
                p += 4
            elif k == R_EXIT:
                vals.append(p + 10)
                p += 10 + VAL.size
            else:
                raise ValueError('record kind %d' % k)
            out.append((k, q, vals))
        return out

    def reseal_entry(self, e, ln):
        struct.pack_into('<Q', self.b, e, xxh64(bytes(self.b[e + 8:e + ln])))

    def reseal(self):
        b = self.b
        struct.pack_into('<Q', b, 32, xxh64(bytes(b[HDR.size:])))
        struct.pack_into('<Q', b, 24, 0)
        struct.pack_into('<Q', b, 24, xxh64(bytes(b[:HDR.size])))


def normalized(c, e, ln, pre=True):
    """An entry as its bytes without the hash and, unless pinned, the module's base; without
    the values' layout predicates if not pre."""
    x = bytearray(c.b[e + 8:e + ln])
    if not ENT.unpack_from(c.b, e)[10] & E_PIN:
        x[24 - 8:32 - 8] = bytes(8)
    if not pre:
        for kind, q, vals in c.records(e):
            for v in vals:
                x[v + 1 - e - 8] = 0
    return bytes(x)


def describe(c, e, at):
    """The record holding byte at of an entry (offset after the hash)."""
    p = e + 8 + at
    last = None
    for kind, q, vals in c.records(e):
        if q - 1 > p:
            break
        last = (kind, q)
    return 'record kind %s at +%d' % (last[0], p - last[1]) if last else 'header or source bytes'


def cmd_check(path):
    c = Cache(open(path, 'rb').read())
    why = c.problems()
    n_rec = sum(len(c.records(e)) for _, e, _ in c.entries()) if why is None else 0
    print('%s: %s, %d entries, %d records, %d bytes' % (path, why or 'ok', c.num, n_rec, len(c.b)))
    return 1 if why else 0


def cmd_diff(a, b):
    ca, cb = Cache(open(a, 'rb').read()), Cache(open(b, 'rb').read())
    for c, p in ((ca, a), (cb, b)):
        why = c.problems()
        if why:
            print('%s: %s' % (p, why))
            return 2
    if bytes(ca.b[ca.key_off:ca.key_off + ca.key_len]) != bytes(cb.b[cb.key_off:cb.key_off + cb.key_len]):
        print('keys differ')
        return 2
    ea = {k: (e, ln) for k, e, ln in ca.entries()}
    same = differ = flags = prediff = 0
    shown = 0
    for k, e, ln in cb.entries():
        if k not in ea:
            continue
        x, y = normalized(ca, *ea[k]), normalized(cb, e, ln)
        fa, fb = ENT.unpack_from(ca.b, ea[k][0]), ENT.unpack_from(cb.b, e)
        if fa[5:7] != fb[5:7] or fa[9] != fb[9]:
            flags += 1  # built under other initial flags or AVX-512 state: not comparable
        elif x == y:
            same += 1
        elif normalized(ca, *ea[k], pre=False) == normalized(cb, e, ln, pre=False):
            prediff += 1  # only a predicate differs: a miss in the other layout, not a wrong hit
        else:
            differ += 1
            if shown < 8:
                shown += 1
                if len(x) == len(y):
                    at = next(i for i in range(len(x)) if x[i] != y[i])
                    where = '%s, %s / %s' % (describe(ca, ea[k][0], at), x[at:at + 12].hex(), y[at:at + 12].hex())
                else:
                    where = 'length %d/%d' % (len(x), len(y))
                print('DIFF module %016x offset %#x: %s' % (k[0], k[1], where))
    print('RESULT twolayout common %d same %d predicates %d differ %d other-flags %d' % (
        same + prediff + differ + flags, same, prediff, differ, flags))
    return 1 if differ else 0


def first_entry_with(c, kinds):
    for k, e, ln in c.entries():
        for kind, q, vals in c.records(e):
            if kind in kinds and (vals or kind == R_RAW):
                return e, ln, kind, q, vals
    raise SystemExit('no entry with records %s' % (kinds,))


def cmd_corrupt(path, mode, out):
    c = Cache(open(path, 'rb').read())
    b = c.b
    e0, ln0 = c.entry_at(c.num // 2)
    if mode == 'flip-body':          # no reseal: the body hash
        b[e0 + ENT.size] ^= 0xff
    elif mode == 'truncate':
        del b[len(b) // 2:]
    elif mode == 'empty':
        del b[:]
    elif mode == 'garbage':
        b[:] = bytes((i * 131 + 7) & 0xff for i in range(len(b)))
    elif mode == 'version':          # resealed: the version check
        struct.pack_into('<I', b, 8, VERSION + 1)
        c.reseal()
    elif mode == 'key':              # resealed: the key compare
        b[c.key_off + c.key_len - 2] ^= 0x01
        c.reseal()
    elif mode == 'entry-hash':       # body resealed, entry not: that entry's hash
        for k, e, ln in c.entries():
            b[e + ln - 1] ^= 0xff
        c.reseal()
    elif mode == 'entry-len':        # entry and body resealed: the length check
        for k, e, ln in c.entries():
            struct.pack_into('<I', b, e + 48, struct.unpack_from('<I', b, e + 48)[0] + 1)
            c.reseal_entry(e, ln)
        c.reseal()
    elif mode == 'index-off':        # resealed: index bounds
        for i in range(c.num):
            struct.pack_into('<Q', b, c.index_off + i * IDX.size + 16, c.data_len + 8)
        c.reseal()
    elif mode == 'record-kind':      # resealed: the record parser
        for k, e, ln in c.entries():
            b[e + ENT.size + ENT.unpack_from(b, e)[7]] = 0x7f
            c.reseal_entry(e, ln)
        c.reseal()
    elif mode == 'predicate':        # resealed: every value's layout predicates flipped
        for k, e, ln in c.entries():
            for kind, q, vals in c.records(e):
                for v in vals:
                    b[v + 1] ^= 0x1
            c.reseal_entry(e, ln)
        c.reseal()
    elif mode == 'value-class':      # resealed: an unknown value class
        for k, e, ln in c.entries():
            for kind, q, vals in c.records(e):
                for v in vals:
                    b[v] = 0x7f
            c.reseal_entry(e, ln)
        c.reseal()
    else:
        raise SystemExit('unknown mode ' + mode)
    open(out, 'wb').write(bytes(b))
    return 0


def cmd_selftest():
    want = {b'': 0xEF46DB3751D8E999, b'a': 0xD24EC4F1A98C6E5B, b'abc': 0x44BC2CF5AD770999,
            b'Nobody inspects the spammish repetition': 0xFBCEA83C8A378BF1}
    bad = [k for k, v in want.items() if xxh64(k) != v]
    print('xxh64 selftest', 'FAILED %r' % bad if bad else 'ok')
    return 1 if bad else 0


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['check'] and len(a) == 2:
        sys.exit(cmd_check(a[1]))
    if a[:1] == ['diff'] and len(a) == 3:
        sys.exit(cmd_diff(a[1], a[2]))
    if a[:1] == ['corrupt'] and len(a) == 4:
        sys.exit(cmd_corrupt(a[1], a[2], a[3]))
    if a[:1] == ['selftest']:
        sys.exit(cmd_selftest())
    sys.exit(__doc__)
