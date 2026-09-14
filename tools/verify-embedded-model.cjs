'use strict';
const fs=require('node:fs'),crypto=require('node:crypto'),zlib=require('node:zlib');
function embeddedBytes(source){
  if(!source.includes('#define SYNAP_CHAKSHU 1')||!source.includes('#define SYNAP_VOICE_FLASH 1'))
    throw Error('Chakshu release must include its internal flash voice model');
  const array=source.match(/alignas\(4\) static const uint8_t DATA\[\]=\{([\s\S]*?)\};/);
  if(!array)throw Error('Missing embedded model array');
  return Buffer.from(array[1].match(/0x[0-9a-f]{2}/g).map(Number));
}
function verify(binary,source,weights){
  const packed=embeddedBytes(source);
  const contract=fs.readFileSync(require('node:path').join(__dirname,'../firmware/xiao-sense/model-contract.cpp'),'utf8');
  const size=Number(contract.match(/MODEL_BYTES=(\d+);/)[1]),hash=contract.match(/MODEL_SHA256\[\]="([a-f0-9]{64})"/)[1];
  if(weights.length!==size||crypto.createHash('sha256').update(weights).digest('hex')!==hash)
    throw Error('Unverified reference model');
  if(!zlib.inflateRawSync(packed,{maxOutputLength:size}).equals(weights))throw Error('Embedded weights changed');
  if(binary.length>0x330000)throw Error('Chakshu model and firmware exceed the existing OTA slot');
  if(!binary.includes(packed))throw Error('Linked firmware is missing the complete compressed model');
  console.log(`Verified embedded voice model in OTA image: ${binary.length}/3342336 bytes, ${packed.length} model bytes`);
}
if(require.main===module){
  const [binary,source,weights]=process.argv.slice(2);
  verify(fs.readFileSync(binary),fs.readFileSync(source,'utf8'),fs.readFileSync(weights));
}
module.exports={embeddedBytes,verify};
