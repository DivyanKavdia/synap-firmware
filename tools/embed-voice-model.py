"""Embed a pinned, losslessly compressed MultiNet command pack in Chakshu."""
import hashlib
import pathlib
import re
import struct
import sys
import zlib
try:
    from zopfli.zlib import compress as zopfli_compress
except ImportError:
    zopfli_compress = None

EXPECTED_MODELS = {
    'mn5q8_en': {
        '_MODEL_INFO_':'2488263ce5dd4d27a50d07604792e233f2c248c6',
        'mn5q8_data':'ccafc8b30bc5cd6cb8cc103cccc943959fc688eb',
        'mn5q8_index':'f17c77e331bd51e88d566db7f80aacb821bef153',
    },
}


def git_blob_sha(data):
    return hashlib.sha1(f'blob {len(data)}\0'.encode()+data).hexdigest()


def verify_pack(weights):
    if len(weights)<4:
        raise ValueError('Voice model pack is truncated')
    count=struct.unpack_from('<I',weights,0)[0]
    if count!=len(EXPECTED_MODELS):
        raise ValueError('Voice model pack has unexpected model count')
    at=4; files=[]
    for expected_model,expected_files in EXPECTED_MODELS.items():
        if at+36>len(weights):
            raise ValueError('Voice model header is truncated')
        raw_name,file_count=struct.unpack_from('<32sI',weights,at);at+=36
        model=raw_name.split(b'\0',1)[0].decode('ascii')
        if model!=expected_model or file_count!=len(expected_files):
            raise ValueError('Voice model pack has unexpected model metadata')
        for expected_name,expected_sha in expected_files.items():
            if at+40>len(weights):
                raise ValueError('Voice model file table is truncated')
            raw_file,offset,size=struct.unpack_from('<32sII',weights,at);at+=40
            name=raw_file.split(b'\0',1)[0].decode('ascii')
            if name!=expected_name:
                raise ValueError('Voice model file order changed')
            files.append((offset,size,expected_sha))
    cursor=at
    for offset,size,expected_sha in files:
        if offset!=cursor or offset+size>len(weights):
            raise ValueError('Voice model data layout is invalid')
        data=weights[offset:offset+size]
        if git_blob_sha(data)!=expected_sha:
            raise ValueError('Voice model file content changed')
        cursor+=size
    if cursor!=len(weights):
        raise ValueError('Voice model pack has trailing data')


def embed(sketch, model):
    source = sketch.read_text()
    if '#define SYNAP_CHAKSHU 1' not in source or 'SYNAP-CHAKSHU-OTA-ID-V3' not in source:
        raise ValueError('Embedded voice weights are only supported on Chakshu')
    marker = '// SYNAP_EMBEDDED_VOICE_MODEL'
    if source.count(marker) != 1 or source.count('#define SYNAP_VOICE_FLASH 0') != 1:
        raise ValueError('Missing or already populated Chakshu model anchor')
    weights = model.read_bytes()
    verify_pack(weights)
    size=len(weights);digest=hashlib.sha256(weights).hexdigest()
    if len(re.findall(r'MODEL_BYTES=\d+;',source))!=1 or len(re.findall(r'MODEL_SHA256\[\]="[a-f0-9]{64}"',source))!=1:
        raise ValueError('Missing unique Chakshu model contract')
    source=re.sub(r'MODEL_BYTES=\d+;',f'MODEL_BYTES={size};',source,count=1)
    source=re.sub(r'MODEL_SHA256\[\]="[a-f0-9]{64}"',f'MODEL_SHA256[]="{digest}"',source,count=1)
    def deflate(strategy):
        compressor = zlib.compressobj(level=9,method=zlib.DEFLATED,wbits=-15,memLevel=9,strategy=strategy)
        return compressor.compress(weights) + compressor.flush()
    strategies = (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED, zlib.Z_RLE, zlib.Z_HUFFMAN_ONLY, zlib.Z_FIXED)
    candidates = [deflate(strategy) for strategy in strategies]
    if zopfli_compress is not None:
        wrapped = zopfli_compress(weights, numiterations=10, blocksplitting=1, blocksplittinglast=0, blocksplittingmax=15)
        if len(wrapped) < 7:
            raise ValueError('Zopfli returned a truncated zlib stream')
        candidates.append(wrapped[2:-4])  # Strip RFC1950 wrapper; runtime expects raw RFC1951 DEFLATE.
    packed = min(candidates, key=len)
    if zlib.decompress(packed, -15) != weights:
        raise ValueError('Lossless voice model round trip failed')
    if len(packed) > 1580000:
        raise ValueError('Compressed model exceeds the release size budget')
    array = 'alignas(4) static const uint8_t DATA[]={\n' + ''.join(
        ','.join(f'0x{b:02x}' for b in packed[i:i + 24]) + ',\n'
        for i in range(0, len(packed), 24)
    ) + '};'
    source = source.replace(marker, array).replace('#define SYNAP_VOICE_FLASH 0', '#define SYNAP_VOICE_FLASH 1')
    sketch.write_text(source)
    print(f'Embedded Chakshu production command model: {len(packed)} flash bytes -> {size} identical PSRAM bytes; SHA-256 {digest}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('Usage: embed-voice-model.py <prepared-chakshu.ino> <srmodels.bin>')
    embed(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
