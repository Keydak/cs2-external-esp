import struct
import zstandard

# KV3 binary node types
NULL, BOOLEAN, INT64, UINT64, DOUBLE, STRING, BLOB, ARRAY, OBJECT, ARRAY_TYPED = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
INT32, UINT32, TRUE, FALSE, I64_ZERO, I64_ONE, D_ZERO, D_ONE = 11, 12, 13, 14, 15, 16, 17, 18
FLOAT, INT16, UINT16, U32_BYTE, I32_BYTE, ARRAY_BYTE_LEN, ARRAY_AUX = 19, 20, 21, 22, 23, 24, 25


class Buffer:
    def __init__(self, data, counts, offset=0):
        b1, b2, b4, b8 = counts
        self.data = data
        p = offset
        self.p1 = p; p += b1
        p = (p + 1) & ~1
        self.p2 = p; p += b2 * 2
        p = (p + 3) & ~3
        self.p4 = p; p += b4 * 4
        p = (p + 7) & ~7
        self.p8 = p; p += b8 * 8
        self.end = p
        self.ends = (self.p1 + b1, self.p2 + b2 * 2, self.p4 + b4 * 4, self.p8 + b8 * 8)
        self.start1 = self.p1

    def u8(self):
        v = self.data[self.p1]; self.p1 += 1; return v
    def i8(self):
        v = struct.unpack_from('<b', self.data, self.p1)[0]; self.p1 += 1; return v
    def i16(self):
        v = struct.unpack_from('<h', self.data, self.p2)[0]; self.p2 += 2; return v
    def u16(self):
        v = struct.unpack_from('<H', self.data, self.p2)[0]; self.p2 += 2; return v
    def i32(self):
        v = struct.unpack_from('<i', self.data, self.p4)[0]; self.p4 += 4; return v
    def u32(self):
        v = struct.unpack_from('<I', self.data, self.p4)[0]; self.p4 += 4; return v
    def f32(self):
        v = struct.unpack_from('<f', self.data, self.p4)[0]; self.p4 += 4; return v
    def i64(self):
        v = struct.unpack_from('<q', self.data, self.p8)[0]; self.p8 += 8; return v
    def u64(self):
        v = struct.unpack_from('<Q', self.data, self.p8)[0]; self.p8 += 8; return v
    def f64(self):
        v = struct.unpack_from('<d', self.data, self.p8)[0]; self.p8 += 8; return v

    def remaining(self):
        return (self.ends[0] - self.p1, self.ends[1] - self.p2, self.ends[2] - self.p4, self.ends[3] - self.p8)


class KV3:
    def __init__(self, data):
        magic = data[:4]
        assert magic == b'\x053VK', magic
        h = struct.unpack_from('<IHHiiiiHHiiiiii' + 'iiii' + 'iiii' + 'iiii', data, 20)
        (self.compression, dict_id, frame_size, b1, b4, b8, count_types, objs, arrays,
         unc_total, comp_total, block_count, blob_bytes, b2, block_sizes_bytes,
         unc1, comp1, unc2, comp2,
         b1_2, b2_2, b4_2, b8_2,
         unk13, objs_2, arrays_2, unk16) = h
        assert self.compression == 2, 'only zstd supported'

        pos = 120
        zd = zstandard.ZstdDecompressor()
        buf1 = zd.decompress(data[pos:pos + comp1], max_output_size=unc1); pos += comp1
        buf2 = zd.decompress(data[pos:pos + comp2], max_output_size=unc2); pos += comp2

        # Buffer 1: bytes (strings live here), shorts, ints, doubles
        self.aux = Buffer(buf1, (b1, b2, b4, b8))
        string_count = self.aux.i32()
        self.strings = []
        for _ in range(string_count):
            end = buf1.index(b'\x00', self.aux.p1)
            self.strings.append(buf1[self.aux.p1:end].decode('utf-8', 'replace'))
            self.aux.p1 = end + 1

        # Buffer 2: object lengths, bytes, shorts, ints, doubles, types, blob sizes, trailer
        self.object_lengths = struct.unpack_from(f'<{objs_2}i', buf2, 0)
        self.object_index = 0
        self.main = Buffer(buf2, (b1_2, b2_2, b4_2, b8_2), offset=objs_2 * 4)
        p = self.main.end
        self.types = buf2[p:p + count_types]; p += count_types
        self.type_pos = 0
        self.blob_sizes = struct.unpack_from(f'<{block_count}I', buf2, p); p += block_count * 4
        trailer = struct.unpack_from('<I', buf2, p)[0]
        assert trailer == 0xFFEEDD00, hex(trailer)

        self.blobs = b''
        if block_count:
            self.blobs = zd.decompress(data[pos:pos + (comp_total - comp1 - comp2)], max_output_size=blob_bytes)
        self.blob_pos = 0
        self.blob_index = 0

        self.current = self.main
        self.root = self.parse_value(*self.read_type())

        self.leftover = {
            'main': self.main.remaining(),
            'aux': self.aux.remaining(),
            'types': len(self.types) - self.type_pos,
            'objects': len(self.object_lengths) - self.object_index,
            'blobs': len(self.blob_sizes) - self.blob_index,
        }

    def read_type(self):
        t = self.types[self.type_pos]; self.type_pos += 1
        flag = 0
        if t & 0x80:
            t &= 0x3F
            flag = self.types[self.type_pos]; self.type_pos += 1
        return t, flag

    def parse_value(self, t, flag):
        b = self.current
        if t == NULL: return None
        if t == TRUE: return True
        if t == FALSE: return False
        if t == I64_ZERO: return 0
        if t == I64_ONE: return 1
        if t == D_ZERO: return 0.0
        if t == D_ONE: return 1.0
        if t == BOOLEAN: return b.u8() == 1
        if t == I32_BYTE: return b.i8()
        if t == U32_BYTE: return b.u8()
        if t == INT16: return b.i16()
        if t == UINT16: return b.u16()
        if t == INT32: return b.i32()
        if t == UINT32: return b.u32()
        if t == FLOAT: return b.f32()
        if t == INT64: return b.i64()
        if t == UINT64: return b.u64()
        if t == DOUBLE: return b.f64()
        if t == STRING:
            i = b.i32()
            return '' if i == -1 else self.strings[i]
        if t == BLOB:
            size = self.blob_sizes[self.blob_index]; self.blob_index += 1
            blob = self.blobs[self.blob_pos:self.blob_pos + size]; self.blob_pos += size
            return blob
        if t == ARRAY:
            count = b.i32()
            return [self.parse_value(*self.read_type()) for _ in range(count)]
        if t == ARRAY_TYPED:
            count = b.i32()
            st, sf = self.read_type()
            return [self.parse_value(st, sf) for _ in range(count)]
        if t == ARRAY_BYTE_LEN:
            count = b.u8()
            st, sf = self.read_type()
            return [self.parse_value(st, sf) for _ in range(count)]
        if t == ARRAY_AUX:
            count = b.u8()
            st, sf = self.read_type()
            previous, self.current = self.current, (self.aux if self.current is self.main else self.main)
            values = [self.parse_value(st, sf) for _ in range(count)]
            self.current = previous
            return values
        if t == OBJECT:
            count = self.object_lengths[self.object_index]; self.object_index += 1
            obj = {}
            for _ in range(count):
                name_index = b.i32()
                name = self.strings[name_index] if name_index >= 0 else ''
                obj[name] = self.parse_value(*self.read_type())
            return obj
        raise ValueError(f'unknown type {t} at type index {self.type_pos}')
