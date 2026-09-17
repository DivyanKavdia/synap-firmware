"""Embed verified, losslessly compressed weights in the Chakshu OTA application."""
import hashlib
import pathlib
import re
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent


def embed(sketch, model):
    source = sketch.read_text()
    contract = (ROOT / 'firmware/xiao-sense/model-contract.cpp').read_text()
    size = int(re.search(r'MODEL_BYTES=(\d+);', contract)[1])
    digest = re.search(r'MODEL_SHA256\[\]="([a-f0-9]{64})"', contract)[1]
    if '#define SYNAP_CHAKSHU 1' not in source or 'SYNAP-CHAKSHU-OTA-ID-V3' not in source:
        raise ValueError('Embedded voice weights are only supported on Chakshu')
    marker = '// SYNAP_EMBEDDED_VOICE_MODEL'
    if source.count(marker) != 1 or source.count('#define SYNAP_VOICE_FLASH 0') != 1:
        raise ValueError('Missing or already populated Chakshu model anchor')
    weights = model.read_bytes()
    if len(weights) != size or hashlib.sha256(weights).hexdigest() != digest:
        raise ValueError('Voice model does not match the pinned size and SHA-256')
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = compressor.compress(weights) + compressor.flush()
    if zlib.decompress(packed, -15) != weights:
        raise ValueError('Lossless voice model round trip failed')
    if len(packed) > 1600000:
        raise ValueError('Compressed model exceeds the release size budget')
    array = 'alignas(4) static const uint8_t DATA[]={\n' + ''.join(
        ','.join(f'0x{b:02x}' for b in packed[i:i + 24]) + ',\n'
        for i in range(0, len(packed), 24)
    ) + '};'
    source = source.replace(marker, array).replace('#define SYNAP_VOICE_FLASH 0', '#define SYNAP_VOICE_FLASH 1')
    sketch.write_text(source)
    print(f'Embedded Chakshu model: {len(packed)} flash bytes -> {size} identical PSRAM bytes; SHA-256 {digest}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('Usage: embed-voice-model.py <prepared-chakshu.ino> <srmodels.bin>')
    embed(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))