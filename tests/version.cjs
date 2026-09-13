'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const {versionForBuild,TARGETS}=require('../tools/release.cjs');

test('compiled S3 and C3 BLE identities use the same OS1 release name and numeric counter',()=>{
  const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
  for(const target of Object.keys(TARGETS)){
    const prepared=materialize(source,target);
    const identity=prepared.slice(prepared.indexOf('#ifndef SYNAP_BUILD'),prepared.indexOf('// Target marker;'));
    for(const build of [0,1194,65535]){
      const result=nativeTest(`#include <cstdint>\n#include <cstdio>\n${identity}\nint main(){std::puts(SYNAP_FIRMWARE_ID);}\n`,[`-DSYNAP_BUILD=${build}`]);
      const version=build?versionForBuild(build):'synap-os1-build0';
      assert.equal(result.trim(),`SYNAP-FW:${target}:${version}:${build}`);
    }
  }
  for(const build of [0,503,65536,1194.5,'1194',NaN])assert.throws(()=>versionForBuild(build),/counter/);
});
