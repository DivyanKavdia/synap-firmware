'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const target='esp32s3-fh4r2-qspi-4m',version='1.0.0',slotSize=0x140000;
function validate(bytes,build) {
  if(!Number.isInteger(build)||build<=503||build>65535) throw Error('Invalid build counter');
  if(bytes.length<288||bytes.length>slotSize) throw Error('Application exceeds default OTA slot or is truncated');
  if(bytes[0]!==0xe9||bytes.readUInt16LE(12)!==9||bytes.readUInt32LE(32)!==0xabcd5432) throw Error('Not an ESP32-S3 application image');
  for(const marker of ['SYNAP-ESP32S3-OTA-ID-V3',`SYNAP-FW:${target}:${version}:${build}\0`]) {
    if(!bytes.includes(Buffer.from(marker))) throw Error('Missing exact compatibility/build marker');
  }
  return crypto.createHash('sha256').update(bytes).digest('hex');
}
if(require.main===module) {
  const [input,number,commit,out]=process.argv.slice(2),build=Number(number);
  if(!/^[0-9a-f]{40}$/.test(commit)) throw Error('Invalid source commit');
  const bytes=fs.readFileSync(input),sha256=validate(bytes,build);
  const artifact=`builds/${build}-${sha256}.bin`;
  const manifest={schema:1,version,build,target,protocol:3,chip:9,flashBytes:4194304,psramBytes:2097152,
    partition:'default',size:bytes.length,sha256,commit,identity:`SYNAP-FW:${target}:${version}:${build}`,
    url:`https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/ota-releases/${artifact}`};
  fs.mkdirSync(out,{recursive:true});
  fs.writeFileSync(path.join(out,'firmware.bin'),bytes);
  fs.writeFileSync(path.join(out,'latest.json'),JSON.stringify(manifest,null,2)+'\n');
  const source=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8').replace(/^#define SYNAP_BUILD \d+$/m,`#define SYNAP_BUILD ${build}`);
  fs.writeFileSync(path.join(out,'synap_esp32s3.ino'),source);
  console.log(`Validated ${version} build ${build}: ${bytes.length}/${slotSize} bytes; ${sha256}`);
}
module.exports={validate,target,version,slotSize};
