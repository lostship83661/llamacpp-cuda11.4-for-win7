
import struct, sys
path = r'C:\models\Qwen3.6\Qwen3.6-27B-uncensored-heretic-v2-Native-MTP-Preserved-Q4_K_M.gguf'
f = open(path, 'rb')
magic = f.read(4)
print('magic', magic)
ver = struct.unpack('<I', f.read(4))[0]
n_tensors = struct.unpack('<Q', f.read(8))[0]
n_kv = struct.unpack('<Q', f.read(8))[0]
print('version', ver, 'n_tensors', n_tensors, 'n_kv', n_kv)

def read_str(f):
    n = struct.unpack('<Q', f.read(8))[0]
    b = f.read(n)
    return b.decode('utf-8', errors='replace')

types = {
    0:'UINT8',1:'INT8',2:'UINT16',3:'INT16',4:'UINT32',5:'INT32',
    6:'FLOAT32',7:'BOOL',8:'STRING',9:'ARRAY',10:'UINT64',11:'INT64',12:'FLOAT64'
}
scalars = {0:'<B',1:'<b',2:'<H',3:'<h',4:'<I',5:'<i',6:'<f',7:'<?',10:'<Q',11:'<q',12:'<d'}

def read_val(tt):
    if tt == 8:
        return read_str(f)
    if tt == 9:
        et = struct.unpack('<I', f.read(4))[0]
        n = struct.unpack('<Q', f.read(8))[0]
        vals=[]
        if et == 8:
            for _ in range(n): vals.append(read_str(f))
        else:
            sz = struct.calcsize(scalars[et])
            data = f.read(sz*n)
            vals = list(struct.unpack('<'+scalars[et].lstrip('<')*n, data)) if False else None
        return f'ARRAY[{types.get(et,et)} x {n}]'
    if tt in scalars:
        return struct.unpack(scalars[tt], f.read(struct.calcsize(scalars[tt])))[0]
    return f'<unknown type {tt}>'

keys_want = ['general.architecture','general.type','general.name','general.basename','general.size_label',
'qwen35.block_count','qwen35.context_length','qwen35.embedding_length','qwen35.feed_forward_length',
'qwen35.attention.head_count','qwen35.attention.head_count_kv','qwen35.rope.freq_base',
'qwen35.attention.key_length','qwen35.attention.value_length','qwen35.full_attention_interval',
'qwen35.nextn_predict_layers','qwen35.ssm.conv_kernel','qwen35.ssm.state_size','qwen35.ssm.group_count',
'qwen35.ssm.time_step_rank','qwen35.ssm.inner_size','qwen35.rope.dimension_sections','qwen35.rope.dimension_count',
'tokenizer.ggml.model','tokenizer.ggml.tokens','tokenizer.ggml.eos_token_id','tokenizer.ggml.bos_token_id','tokenizer.ggml.padding_token_id',
'tokenizer.chat_template','general.quantization_version','general.file_type',
'general.sampling.top_k','general.sampling.top_p','general.sampling.temp']
for i in range(n_kv):
    key = read_str(f)
    tt = struct.unpack('<I', f.read(4))[0]
    val = read_val(tt)
    if key in keys_want or key.startswith('qwen35.') or key.startswith('general.') or key.startswith('tokenizer.') or key.startswith('nextn'):
        if key == 'tokenizer.chat_template':
            s = val if isinstance(val,str) else str(val)
            print(f'{key}: {types.get(tt,tt)} len={len(s)}')
            print('--- template head ---')
            print(s[:1500])
            print('--- template tail ---')
            print(s[-800:])
        elif key in ('tokenizer.ggml.tokens',):
            print(f'{key}: {types.get(tt,tt)} (array)')
        else:
            print(f'{key}: {types.get(tt,tt)} = {val}')
print('metadata bytes read:', f.tell())
f.close()
