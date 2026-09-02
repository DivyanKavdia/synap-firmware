const {test}=require('node:test'),assert=require('node:assert/strict');
const {validate,target,version,slotSize}=require('../tools/release.cjs');
function image(){const b=Buffer.alloc(512);b[0]=0xe9;b.writeUInt16LE(9,12);b.writeUInt32LE(0xabcd5432,32);
  b.write('SYNAP-ESP32S3-OTA-AUTH-V2',100);b.write(`SYNAP-FW:${target}:${version}:1001\0`,160);return b;}
test('only exact target/build application fits default OTA slots',()=>{
  assert.match(validate(image(),1001),/^[a-f0-9]{64}$/);
  assert.throws(()=>validate(image(),1002),/marker/);
  for(const i of [0,12,32,100,160]){const b=image();b[i]^=1;assert.throws(()=>validate(b,1001));}
  assert.throws(()=>validate(Buffer.alloc(slotSize+1),1001),/slot/);
  assert.throws(()=>validate(image(),65536),/counter/);
});
