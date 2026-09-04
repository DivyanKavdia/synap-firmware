'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {PRIMARY_TARGET,TARGETS,getTarget}=require('./targets.cjs');
const target=PRIMARY_TARGET,version='0.0.1',slotSize=getTarget(PRIMARY_TARGET).slotSize;
const repository='DivyanKavdia/synap-firmware',workflow='.github/workflows/firmware.yml';
const allowedBranches=new Set(['ota-test','ota-releases']);

function validate(bytes,build,targetId=PRIMARY_TARGET) {
  const config=getTarget(targetId);
  if(!Number.isInteger(build)||build<=503||build>65535) throw Error('Invalid build counter');
  if(bytes.length<288||bytes.length>config.slotSize) throw Error('Application exceeds default OTA slot or is truncated');
  if(bytes[0]!==0xe9||bytes.readUInt16LE(12)!==config.chip||bytes.readUInt32LE(32)!==0xabcd5432)
    throw Error(`Not a ${config.board} application image`);
  for(const marker of [config.productMarker,`SYNAP-FW:${config.id}:${version}:${build}\0`]) {
    if(!bytes.includes(Buffer.from(marker))) throw Error('Missing exact compatibility/build marker');
  }
  return crypto.createHash('sha256').update(bytes).digest('hex');
}

function canonicalManifest(m) {
  return JSON.stringify({schema:m.schema,version:m.version,build:m.build,target:m.target,protocol:m.protocol,chip:m.chip,
    flashBytes:m.flashBytes,psramBytes:m.psramBytes,partition:m.partition,size:m.size,sha256:m.sha256,
    commit:m.commit,identity:m.identity,url:m.url,channel:m.channel});
}

function createManifest(bytes,build,commit,branch='ota-test',targetId=PRIMARY_TARGET) {
  const config=getTarget(targetId);
  if(!allowedBranches.has(branch)) throw Error('Invalid release branch');
  if(!/^[0-9a-f]{40}$/.test(commit)) throw Error('Invalid source commit');
  const sha256=validate(bytes,build,targetId),production=branch==='ota-releases',channel=production?'production':'test';
  const artifact=`${config.releasePrefix}builds/${build}-${sha256}.bin`;
  const manifest={schema:production?3:2,version,build,target:config.id,protocol:3,chip:config.chip,
    flashBytes:config.flashBytes,psramBytes:config.psramBytes,partition:config.partition,size:bytes.length,sha256,commit,
    identity:`SYNAP-FW:${config.id}:${version}:${build}`,
    url:`https://raw.githubusercontent.com/${repository}/${branch}/${artifact}`,channel};
  if(production) manifest.provenance={provider:'github-actions',repository,workflow};
  return manifest;
}

if(require.main===module) {
  const [input,number,commit,out,branchArg,sourceArg,targetArg]=process.argv.slice(2),build=Number(number);
  const branch=branchArg||process.env.SYNAP_RELEASE_BRANCH||'ota-test',targetId=targetArg||PRIMARY_TARGET,config=getTarget(targetId);
  const bytes=fs.readFileSync(input),manifest=createManifest(bytes,build,commit,branch,targetId);
  fs.mkdirSync(out,{recursive:true});
  fs.writeFileSync(path.join(out,'firmware.bin'),bytes);
  fs.writeFileSync(path.join(out,'latest.json'),JSON.stringify(manifest,null,2)+'\n');
  const sourcePath=sourceArg||path.join(config.family==='esp32s3'?'synap_esp32s3':'synap_esp32c3',config.sourceName);
  const source=fs.readFileSync(sourcePath,'utf8').replace(/^#define SYNAP_BUILD \d+$/m,`#define SYNAP_BUILD ${build}`);
  fs.writeFileSync(path.join(out,config.sourceName),source);
  fs.writeFileSync(path.join(out,'source.sha256'),crypto.createHash('sha256').update(source).digest('hex')+`  ${config.sourceName}\n`);
  console.log(`Validated synap ${version} build ${build} for ${config.id}/${manifest.channel}: ${bytes.length}/${config.slotSize} bytes; ${manifest.sha256}`);
}

module.exports={validate,createManifest,canonicalManifest,target,version,slotSize,allowedBranches,repository,workflow,PRIMARY_TARGET,TARGETS,getTarget};
