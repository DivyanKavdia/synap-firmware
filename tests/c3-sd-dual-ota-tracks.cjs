'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {TARGETS}=require('../tools/targets.cjs');
const {assemble}=require('../tools/assemble-source.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const {verify,EXPECTED}=require('../tools/verify-c3-sd-layout.cjs');
const original='esp32c3-supermini-4m',modern='esp32c3-supermini-4m-sd';
const legacy=TARGETS[original],next=TARGETS[modern];
test('shipped C3 OTA identity, partition and size stay unchanged',()=>{
  assert.equal(legacy.partition,'default');
  assert.equal(legacy.slotSize,1310720);
  assert.equal(legacy.productMarker,'SYNAP-ESP32C3-OTA-ID-V3');
  assert.equal(legacy.manifestPath,'targets/esp32c3-supermini-4m/latest.json');
  assert.equal(next.slotSize,0x1E0000);
  assert.equal(next.partition,'c3-sd-dual-ota');
  assert.notEqual(next.productMarker,legacy.productMarker);
  assert.notEqual(next.manifestPath,legacy.manifestPath);
  assert.equal(next.moduleId,legacy.moduleId,'retain existing BLE media/PWA module protocol');
  assert.equal(next.flashBytes,0x400000);
});
test('C3+SD materialization targets unique firmware marker and leaves Chakshu/S3 absent',()=>{
  const c3=materialize(assemble(),modern);
  const old=materialize(assemble(),original);
  assert.match(c3,/SYNAP-ESP32C3-SD-OTA-ID-V1/);
  assert.match(c3,/SYNAP-FW:esp32c3-supermini-4m-sd:/);
  assert.doesNotMatch(c3,/SYNAP-ESP32C3-OTA-ID-V3/);
  assert.doesNotMatch(old,/SYNAP-ESP32C3-SD-OTA-ID-V1/);
  assert.match(c3,/#define SYNAP_CHAKSHU 0/);
  assert.doesNotMatch(c3,/#define SYNAP_C3_WIFI_UPLOAD_ENABLED/);
  assert.doesNotMatch(old,/#define SYNAP_C3_WIFI_UPLOAD_ENABLED/);
  assert(!c3.includes('WiFiClientSecure'), 'new C3+SD firmware must be BLE-only');
});
test('new C3+SD factory table has two large OTA slots without flash overlap',()=>{
  const csv=fs.readFileSync('partitions/c3-sd-dual-ota.csv','utf8');
  const rows=csv.split(/\r?\n/).filter(x=>x.trim()&&!x.trim().startsWith('#'));
  assert.equal(rows.length,5);
  const layout=rows.map(row=>{
    const [label,type,subtype,offset,size]=row.split(',').map(x=>x.trim());
    return {label,type,subtype,offset:Number(offset),size:Number(size)};
  });
  for(const e of layout){const want=EXPECTED[e.label];assert(want,e.label);assert.equal(e.offset,want.offset);assert.equal(e.size,want.size);}
  const binary=Buffer.alloc(0xc00,0xff);
  const types={data:1,app:0},subtypes={nvs:2,ota:0,phy:1,ota_0:0x10,ota_1:0x11};
  layout.forEach((e,i)=>{
    const off=i*32;binary.writeUInt16LE(0x50aa,off);
    binary[off+2]=types[e.type];binary[off+3]=subtypes[e.subtype];
    binary.writeUInt32LE(e.offset,off+4);binary.writeUInt32LE(e.size,off+8);
    binary.fill(0,off+12,off+28);
    binary.write(e.label,off+12,'ascii');
  });
  assert.equal(verify(binary).length,5);
});

test('large-slot OTA specialization is same target as already-installed 2119 C3',()=>{
  const upgrade=TARGETS['esp32c3-supermini-4m-sd-ota'];
  assert.equal(upgrade.slotSize,1966080);
  assert.equal(upgrade.firmwareTarget,original);
  assert.equal(upgrade.productMarker,legacy.productMarker);
  assert.notEqual(upgrade.manifestPath,legacy.manifestPath);
  assert.notEqual(upgrade.manifestPath,next.manifestPath);
  const sketch=materialize(assemble(),upgrade.id);
  assert.match(sketch,/SYNAP-ESP32C3-OTA-ID-V3/);
  assert.doesNotMatch(sketch,/#define SYNAP_C3_WIFI_UPLOAD_ENABLED/);
  assert.match(sketch,/SYNAP-FW:esp32c3-supermini-4m:/);
  assert.doesNotMatch(sketch,/SYNAP-FW:esp32c3-supermini-4m-sd-ota:/);
  assert.doesNotMatch(sketch,/SYNAP-ESP32C3-SD-OTA-ID-V1/);
});
test('only 1.875MiB OTA slots can receive large-slot C3+SD BLE binary',()=>{
  const {createManifest,validate}=require('../tools/release.cjs');
  const upgrade=TARGETS['esp32c3-supermini-4m-sd-ota'];
  const build=2123;
  const bytes=Buffer.alloc(1500000,0);
  bytes[0]=0xe9;bytes.writeUInt16LE(5,12);bytes.writeUInt32LE(0xABCD5432,32);
  bytes.write(upgrade.productMarker,64);
  bytes.write('SYNAP-FW:'+original+':synap-os1-build'+build+':'+build+String.fromCharCode(0),100);
  assert.doesNotThrow(()=>validate(bytes,build,upgrade.id));
  assert.throws(()=>validate(bytes,build,original),/OTA slot/);
  const m=createManifest(bytes,build,'a'.repeat(40),'ota-test',upgrade.id);
  assert.equal(m.identity,'SYNAP-FW:'+original+':synap-os1-build'+build+':'+build);
  assert.equal(m.target,upgrade.id);
});
