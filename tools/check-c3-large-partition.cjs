'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const file='firmware/partitions/odyssey-c3-large-ota.csv';
const rows=fs.readFileSync(file,'utf8').split(/\r?\n/).map(line=>line.trim())
  .filter(line=>line&&!line.startsWith('#'))
  .map(line=>line.split(',').map(part=>part.trim()));
const expected=[
  ['nvs','data','nvs',0x9000,0x5000],
  ['otadata','data','ota',0xE000,0x2000],
  ['app0','app','ota_0',0x10000,0x1C0000],
  ['app1','app','ota_1',0x1D0000,0x1C0000],
  ['spiffs','data','spiffs',0x390000,0x60000],
  ['coredump','data','coredump',0x3F0000,0x10000]
];
assert.equal(rows.length,expected.length,'Expected exact partition count');
let previousEnd=0x9000;
for(let i=0;i<rows.length;i++){
  const [name,type,subtype,offset,size]=rows[i],e=expected[i];
  assert.deepEqual([name,type,subtype,Number(offset),Number(size)],e,'Unexpected partition '+name);
  assert.equal(Number(offset)%0x1000,0,'Partition offset is not sector-aligned');
  if(type==='app')assert.equal(Number(offset)%0x10000,0,'App offset is not 64 KiB aligned');
  assert.ok(Number(offset)>=previousEnd,'Overlapping partition '+name);
  previousEnd=Number(offset)+Number(size);
  assert.ok(previousEnd<=0x400000,'Partition exceeds 4 MiB flash');
}
assert.equal(previousEnd,0x400000);
assert.equal(expected[2][4],1835008);
assert.equal(expected[3][4],1835008);
console.log('PASS: Odyssey C3 migration layout: 1.75 MiB x2, 384 KiB SPIFFS, NVS and coredump preserved.');
