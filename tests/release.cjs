const {test}=require('node:test'),assert=require('node:assert/strict');
const {validate,createManifest,canonicalManifest,target,version,slotSize,repository,workflow,TARGETS}=require('../tools/release.cjs');
function image(targetId=target,build=1001){const config=TARGETS[targetId],b=Buffer.alloc(512);b[0]=0xe9;b.writeUInt16LE(config.chip,12);b.writeUInt32LE(0xabcd5432,32);
  b.write(config.productMarker,80);b.write(`SYNAP-FW:${targetId}:${version}:${build}\0`,160);return b;}

test('S3 and C3 images are exact-target bound and fit default OTA slots',()=>{
  for(const targetId of Object.keys(TARGETS)){
    assert.match(validate(image(targetId),1001,targetId),/^[a-f0-9]{64}$/);
    assert.throws(()=>validate(image(targetId),1002,targetId),/marker/);
    const other=Object.keys(TARGETS).find(id=>id!==targetId);
    assert.throws(()=>validate(image(targetId),1001,other),/application image|marker/);
  }
  assert.throws(()=>validate(Buffer.alloc(slotSize+1),1001,target),/slot/);
  assert.throws(()=>validate(image(target),65536,target),/counter/);
});

test('production publishes one build with target-specific URLs and GitHub provenance',()=>{
  const commit='a'.repeat(40),manifests={};
  for(const targetId of Object.keys(TARGETS)){
    const testManifest=createManifest(image(targetId),1001,commit,'ota-test',targetId);
    const prod=createManifest(image(targetId),1001,commit,'ota-releases',targetId);
    manifests[targetId]=prod;
    assert.equal(testManifest.channel,'test');assert.equal(testManifest.schema,2);
    assert.equal(prod.channel,'production');assert.equal(prod.schema,3);assert.equal(prod.target,targetId);
    assert.deepEqual(prod.provenance,{provider:'github-actions',repository,workflow});assert.equal(prod.signing,undefined);
    assert.notEqual(canonicalManifest(testManifest),canonicalManifest(prod));
  }
  assert.match(manifests[target].url,/\/ota-releases\/builds\/1001-/,'S3 root path remains backward compatible');
  assert.match(manifests['esp32c3-supermini-4m'].url,/\/ota-releases\/targets\/esp32c3-supermini-4m\/builds\/1001-/);
  assert.equal(manifests[target].build,manifests['esp32c3-supermini-4m'].build);
  assert.throws(()=>createManifest(image(target),1001,commit,'main',target),/branch/);
});
