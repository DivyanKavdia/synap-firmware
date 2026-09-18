'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),zlib=require('node:zlib');
const {execFileSync}=require('node:child_process');
const {materialize}=require('../tools/materialize-target.cjs');
const {embeddedBytes,verify}=require('../tools/verify-embedded-model.cjs');
const shared=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const target='xiao-esp32s3-sense-8m';
test('embedded model tooling refuses C3/S3 and corrupt Chakshu weights before changing source',()=>{
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'chakshu-pack-'));
  try {
    const model=path.join(dir,'bad.bin');fs.writeFileSync(model,Buffer.alloc(20));
    for(const id of ['esp32c3-supermini-4m','esp32s3-fh4r2-qspi-4m',target]){
      const source=materialize(shared,id),sketch=path.join(dir,'sketch.ino');fs.writeFileSync(sketch,source);
      assert.throws(()=>execFileSync('python3',['tools/embed-voice-model.py',sketch,model],{stdio:'pipe'}));
      assert.equal(fs.readFileSync(sketch,'utf8'),source);
      if(id!==target)assert(!source.includes('ChakshuFlashModel'));
    }
  } finally {fs.rmSync(dir,{recursive:true,force:true});}
});
test('pinned model round-trips through prepared source, is required in linked image and fits the OTA budget',
  {skip:!process.env.SYNAP_MODEL_PACK},()=>{
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'chakshu-pack-'));
  try {
    const model=path.resolve(process.env.SYNAP_MODEL_PACK),weights=fs.readFileSync(model),sketch=path.join(dir,'sketch.ino');
    fs.writeFileSync(sketch,materialize(shared,target));
    execFileSync('python3',['tools/embed-voice-model.py',sketch,model]);
    const source=fs.readFileSync(sketch,'utf8'),packed=embeddedBytes(source);
    assert(packed.length<1500000);assert(zlib.inflateRawSync(packed).equals(weights));
    const image=Buffer.concat([Buffer.alloc(1300000),packed]);verify(image,source,weights);
    assert.throws(()=>verify(Buffer.alloc(1300000),source,weights),/missing/);
    const overflow=0x330000-image.length+1;
    assert(overflow>0);assert.throws(()=>verify(Buffer.concat([image,Buffer.alloc(overflow)]),source,weights),/OTA slot/);
    assert.throws(()=>execFileSync('python3',['tools/embed-voice-model.py',sketch,model],{stdio:'pipe'}));
  } finally {fs.rmSync(dir,{recursive:true,force:true});}
});