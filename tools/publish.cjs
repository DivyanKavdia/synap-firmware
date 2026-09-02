'use strict';
// Runs only in the protected publish job with this repository's GITHUB_TOKEN.
const fs=require('node:fs'),{execFileSync}=require('node:child_process');
const {validate}=require('./release.cjs');
const repo='DivyanKavdia/synap-firmware',branch='ota-releases';
function api(endpoint,method='GET',body) {
  return JSON.parse(execFileSync('gh',['api',`repos/${repo}/${endpoint}`,'--method',method,...(body?['--input','-']:[])],
    {input:body?JSON.stringify(body):undefined,encoding:'utf8'}));
}
const manifest=JSON.parse(fs.readFileSync('bundle/latest.json','utf8'));
const binary=fs.readFileSync('bundle/firmware.bin');
if(validate(binary,manifest.build)!==manifest.sha256) throw Error('Artifact hash mismatch');
if(api('commits/main').sha!==manifest.commit) { console.log('Superseded source commit; not advertising an older release.');process.exit(0); }
const branches=api('branches?per_page=100');
const existing=branches.find(b=>b.name===branch);
let base;
if(existing) {
  base=api(`git/commits/${existing.commit.sha}`);
  const old=api(`contents/latest.json?ref=${branch}`);
  const previous=JSON.parse(Buffer.from(old.content,'base64').toString());
  if(previous.build>=manifest.build) {console.log('This or a newer build is already published.');process.exit(0);}
}
const tag=`v${manifest.version}-build.${manifest.build}`;
const releases=api('releases?per_page=100');
if(!releases.some(r=>r.tag_name===tag)) {
  execFileSync('gh',['release','create',tag,'bundle/firmware.bin','bundle/latest.json','bundle/synap_esp32s3.ino',
    '--repo',repo,'--target',manifest.commit,'--title',`Synap ${manifest.version} · build ${manifest.build}`,
    '--notes','ESP32-S3FH4R2: 4 MB flash, 2 MB QSPI PSRAM. Application-only OTA; existing default partition layout. See README for first-time owner authorization.'],{stdio:'inherit'});
}
const blob=api('git/blobs','POST',{content:binary.toString('base64'),encoding:'base64'});
const artifact=new URL(manifest.url).pathname.split('/ota-releases/')[1];
const tree=api('git/trees','POST',{...(base?{base_tree:base.tree.sha}:{}),tree:[
  {path:artifact,mode:'100644',type:'blob',sha:blob.sha},
  {path:'latest.json',mode:'100644',type:'blob',content:JSON.stringify(manifest,null,2)+'\n'}]});
const commit=api('git/commits','POST',{message:`Publish verified build ${manifest.build}`,tree:tree.sha,parents:existing?[existing.commit.sha]:[]});
// Both the manifest and immutable binary become visible together; no forced updates.
if(existing) api(`git/refs/heads/${branch}`,'PATCH',{sha:commit.sha,force:false});
else api('git/refs','POST',{ref:`refs/heads/${branch}`,sha:commit.sha});
console.log(`Published ${tag}: ${manifest.url}`);
