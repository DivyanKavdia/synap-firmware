'use strict';
// Runs only in the repository publish job with this repository's GITHUB_TOKEN.
const fs=require('node:fs'),{execFileSync}=require('node:child_process');
const {validate}=require('./release.cjs');
const repo='DivyanKavdia/synap-firmware',branch=process.env.SYNAP_RELEASE_BRANCH||'ota-test';
if(!['ota-test','ota-releases'].includes(branch))throw Error('Invalid release branch');
function api(endpoint,method='GET',body) {
  return JSON.parse(execFileSync('gh',['api',`repos/${repo}/${endpoint}`,'--method',method,...(body?['--input','-']:[])],
    {input:body?JSON.stringify(body):undefined,encoding:'utf8'}));
}
const manifest=JSON.parse(fs.readFileSync('bundle/latest.json','utf8'));
const binary=fs.readFileSync('bundle/firmware.bin');
if(validate(binary,manifest.build)!==manifest.sha256) throw Error('Artifact hash mismatch');
const production=branch==='ota-releases';
if(manifest.schema!==2||manifest.channel!==(production?'production':'test')||!manifest.url.includes(`/${branch}/builds/`))throw Error('Manifest channel/URL mismatch');
if(production&&(!manifest.signing||manifest.signing.alg!=='ES256'||manifest.signing.keyId!=='prod-2026-01'||
    !/^[A-Za-z0-9+/]{86}==$/.test(manifest.signing.value)))throw Error('Production manifest is not signed');
if(!production&&manifest.signing)throw Error('Test manifests must not carry the production signature');
if(api('commits/main').sha!==manifest.commit) { console.log('Superseded source commit; not advertising an older release.');process.exit(0); }
const branches=api('branches?per_page=100'),existing=branches.find(b=>b.name===branch);let base;
if(existing) {
  base=api(`git/commits/${existing.commit.sha}`);
  const old=api(`contents/latest.json?ref=${branch}`),previous=JSON.parse(Buffer.from(old.content,'base64').toString());
  if(previous.build>=manifest.build) {console.log('This or a newer build is already published.');process.exit(0);}
}
if(production) {
  const tag=`v${manifest.version}-build.${manifest.build}`,releases=api('releases?per_page=100');
  if(!releases.some(r=>r.tag_name===tag)) {
    execFileSync('gh',['release','create',tag,'bundle/firmware.bin','bundle/latest.json','bundle/synap_esp32s3.ino',
      '--repo',repo,'--target',manifest.commit,'--title',`Synap ${manifest.version} · build ${manifest.build}`,
      '--notes','Production-qualified ESP32-S3FH4R2 firmware. Signed manifest, resumable BLE OTA, 4 MB flash, 2 MB QSPI PSRAM, default dual OTA slots.'],{stdio:'inherit'});
  }
}
const blob=api('git/blobs','POST',{content:binary.toString('base64'),encoding:'base64'});
const artifact=new URL(manifest.url).pathname.split(`/${branch}/`)[1];
const tree=api('git/trees','POST',{...(base?{base_tree:base.tree.sha}:{}),tree:[
  {path:artifact,mode:'100644',type:'blob',sha:blob.sha},
  {path:'latest.json',mode:'100644',type:'blob',content:JSON.stringify(manifest,null,2)+'\n'}]});
const commit=api('git/commits','POST',{message:`Publish ${manifest.channel} build ${manifest.build}`,tree:tree.sha,parents:existing?[existing.commit.sha]:[]});
if(existing) api(`git/refs/heads/${branch}`,'PATCH',{sha:commit.sha,force:false});
else api('git/refs','POST',{ref:`refs/heads/${branch}`,sha:commit.sha});
console.log(`Published ${manifest.channel} build ${manifest.build}: ${manifest.url}`);
