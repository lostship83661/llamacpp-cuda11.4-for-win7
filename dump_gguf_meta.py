import struct, sys

def dump(path):
    f = open(path, 'rb')
    magic = f.read(4)
    ver = struct.unpack('<I', f.read(4))[0]
    n_tensors = struct.unpack('<Q', f.read(8))[0]
    n_kv = struct.unpack('<Q', f.read(8))[0]
    print(f'== {path} ==')
    print('magic', magic, 'version', ver, 'n_tensors', n_tensors, 'n_kv', n_kv)

    def read_str(f):
        n = struct.unpack('<Q', f.read(8))[0]
        return f.read(n).decode('utf-8', errors='replace')

    types = {0:'UINT8',1:'INT8',2:'UINT16',3:'INT16',4:'UINT32',5:'INT32',
             6:'FLOAT32',7:'BOOL',8:'STRING',9:'ARRAY',10:'UINT64',11:'INT64',12:'FLOAT64'}
    scalars = {0:'<B',1:'<b',2:'<H',3:'<h',4:'<I',5:'<i',6:'<f',7:'<?',10:'<Q',11:'<q',12:'<d'}

    def read_val(tt):
        if tt == 8:
            return read_str(f)
        if tt == 9:
            et = struct.unpack('<I', f.read(4))[0]
            n = struct.unpack('<Q', f.read(8))[0]
            vals = []
            for _ in range(n):
                if et == 8:
                    vals.append(read_str(f))
                else:
                    vals.append(struct.unpack(scalars[et], f.read(struct.calcsize(scalars[et])))[0])
            if et == 8:
                return 'ARRAY[STRING x %d]: %s' % (n, vals[:6])
            return 'ARRAY[%s x %d]: %s' % (types.get(et, et), n, vals[:12])
        if tt in scalars:
            return struct.unpack(scalars[tt], f.read(struct.calcsize(scalars[tt])))[0]
        return '<unknown type %d>' % tt

    for i in range(n_kv):
        key = read_str(f)
        tt = struct.unpack('<I', f.read(4))[0]
        val = read_val(tt)
        if key == 'tokenizer.chat_template':
            s = val if isinstance(val, str) else str(val)
            print(f'{key}: {types.get(tt,tt)} len={len(s)} (skipped body)')
        else:
            print(f'{key}: {types.get(tt,tt)} = {val}')
    f.close()

for p in sys.argv[1:]:
    dump(p)
