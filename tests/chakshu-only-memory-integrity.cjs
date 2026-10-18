'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const file=p=>fs.readFileSync(p,'utf8');
test('Chakshu SD video includes an incremental CRC and validates finalized file sizes',()=>{
 const source=file('firmware/xiao-sense/sd-recording.cpp');
 assert.match(source,/uint32_t updateVisualCrc\(/);
 assert.match(source,/visualCrc=updateVisualCrc\(visualCrc,slot->bytes,n\)/);
 assert.match(source,/visualCrc32/);
 assert.match(source,/\(unsigned long\)\(~visualCrc\)/);
 assert.match(source,/audio\.size\(\)!=uint64_t\(audioBytes\)\+44u/);
 assert.match(source,/video\.size\(\)!=videoBytes/);
 // CRC32 reference vectors, same reflected polynomial used in the recorder.
 const crc=(data)=>{let c=0xffffffff;for(const b of data){c^=b;for(let k=0;k<8;k++)c=(c>>>1)^((c&1)?0xedb88320:0);}return(~c)>>>0};
 assert.equal(crc(Buffer.from('123456789')),0xcbf43926);
});
test('Chakshu op35 SD diagnostics are RAM-only and do not call FAT or touch the card',()=>{
 const source=file('firmware/xiao-sense/media-transfer.cpp');
 const body=source.split('case 35: {')[1].split('case 14:')[0];
 assert(body);
 assert.match(body,/ChakshuStorage::diagnostics\(bytes,sizeof\(bytes\)\)/);
 const executed=body.split('\n').filter(line=>!line.trim().startsWith('//')).join('\n');
 assert.doesNotMatch(executed,/SD\.|recoverIO|remount|begin\(|unlink|format\(/);
 assert.match(source,/case 17:/); // legacy Chakshu delete remains available (unlike C3).
});
test('Chakshu-only publication never changes other target manifests or the global target index',()=>{
 const wf=file('.github/workflows/firmware.yml');
 const pub=file('tools/publish.cjs');
 const check=file('tools/check-feed.cjs');
 assert.match(wf,/contains\(github\.event\.head_commit\.message, '\[chakshu-only\]'\)/);
 assert.match(wf,/SYNAP_PUBLISH_TARGETS.*xiao-esp32s3-sense-8m/);
 assert.match(pub,/onlyTarget\?artifacts\.filter\(x=>x\.config\.id===onlyTarget\):artifacts/);
 assert.match(pub,/for\(const \{config,manifest,binary\} of selected\)/);
 assert.match(pub,/if\(!onlyTarget\)treeEntries\.push\(\{path:'targets\.json'/);
 assert.match(check,/for\(const \{config,manifest\} of checked\)/);
});
