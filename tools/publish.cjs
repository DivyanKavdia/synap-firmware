'use strict';
// Runs only in the repository publish job with this repository's GITHUB_TOKEN.
const fs=require('node:fs'),path=require('node:path'),{execFileSync}=require('node:child_process');
const {validate,repository,workflow,PRIMARY_TARGET,TARGETS}=require('./release.cjs');
const repo=repository,branch=process.env.SYNAP_RELEASE_BRANCH||'ota-test';
if(!['ota-test','ota-releases'].includes(branch))throw Error('Invalid release branch');

function api(endpoint,method='GET',body) {
  return JSON.parse(execFileSync('gh',['api',`repos/${repo}/${endpoint}`,'--method',method,...(body?['--input','-']:[])],
    {input:body?JSON.stringify(body):undefined,encoding:'utf8'}));
}
function setPublished(value){
  if(process.env.GITHUB_OUTPUT)fs.appendFileSync(process.env.GITHUB_OUTPUT,`published=${value?'true':'false'}\n`);
}

const artifacts=Object.values(TARGETS).map(config=>{
  const dir=path.join('bundle',config.id),manifest=JSON.parse(fs.readFileSync(path.join(dir,'latest.json'),'utf8'));
  const binary=fs.readFileSync(path.join(dir,'firmware.bin'));
  if(manifest.target!==config.id)throw Error(`Bundle target mismatch for ${config.id}`);
  if(validate(binary,manifest.build,config.id)!==manifest.sha256)throw Error(`Artifact hash mismatch for ${config.id}`);
  return {config,dir,manifest,binary};
});
const primary=artifacts.find(x=>x.config.id===PRIMARY_TARGET);
if(!primary)throw Error('Primary S3 release is missing');
const build=primary.manifest.build,commit=primary.manifest.commit,version=primary.manifest.version;
for(const item of artifacts){
  const m=item.manifest,production=branch==='ota-releases';
  if(m.build!==build||m.commit!==commit||m.version!==version)throw Error('All target artifacts must share one build, commit and version');
  if(m.channel!==(production?'production':'test')||!m.url.includes(`/${branch}/`))throw Error(`Manifest channel/URL mismatch for ${m.target}`);
  if(production&&(
    m.schema!==3||m.signing||m.provenance?.provider!=='github-actions'||
    m.provenance?.repository!==repo||m.provenance?.workflow!==workflow
  ))throw Error(`Production provenance declaration invalid for ${m.target}`);
  if(!production&&(m.schema!==2||m.signing||m.provenance))throw Error(`Test trust metadata invalid for ${m.target}`);
}
if(api('commits/main').sha!==commit){setPublished(false);console.log('Superseded source commit; not advertising an older release.');process.exit(0);}

const production=branch==='ota-releases';
const branches=api('branches?per_page=100'),existing=branches.find(b=>b.name===branch);let base;
if(existing){
  base=api(`git/commits/${existing.commit.sha}`);
  const old=api(`contents/latest.json?ref=${branch}`),previous=JSON.parse(Buffer.from(old.content,'base64').toString());
  if(previous.build>=build){setPublished(false);console.log('This or a newer build is already published.');process.exit(0);}
}

if(production){
  const tag=`v${version}-build.${build}`,releases=api('releases?per_page=100');
  if(!releases.some(r=>r.tag_name===tag)){
    const assetDir=path.join('bundle','release-assets');fs.mkdirSync(assetDir,{recursive:true});
    const assets=[];
    for(const {config,dir} of artifacts){
      const stem=config.family;
      const binaryOut=path.join(assetDir,`firmware-${stem}.bin`),manifestOut=path.join(assetDir,`latest-${stem}.json`),sourceOut=path.join(assetDir,config.sourceName),hashOut=path.join(assetDir,`source-${stem}.sha256`);
      fs.copyFileSync(path.join(dir,'firmware.bin'),binaryOut);fs.copyFileSync(path.join(dir,'latest.json'),manifestOut);
      fs.copyFileSync(path.join(dir,config.sourceName),sourceOut);fs.copyFileSync(path.join(dir,'source.sha256'),hashOut);
      assets.push(binaryOut,manifestOut,sourceOut,hashOut);
    }
    execFileSync('gh',['release','create',tag,...assets,'--repo',repo,'--target',commit,
      '--title',`synap ${version} · build ${build}`,
      '--notes','Production-qualified multi-target pendant firmware. Includes ESP32-S3 SuperMini and ESP32-C3 SuperMini artifacts with exact prepared source, GitHub OIDC provenance, target-bound manifests, resumable BLE OTA and default dual OTA slots.'],{stdio:'inherit'});
  }
}

const treeEntries=[];
for(const {config,manifest,binary} of artifacts){
  const blob=api('git/blobs','POST',{content:binary.toString('base64'),encoding:'base64'});
  const artifactPath=new URL(manifest.url).pathname.split(`/${branch}/`)[1];
  treeEntries.push({path:artifactPath,mode:'100644',type:'blob',sha:blob.sha});
  treeEntries.push({path:config.manifestPath,mode:'100644',type:'blob',content:JSON.stringify(manifest,null,2)+'\n'});
}
const targetIndex={schema:1,version,build,channel:production?'production':'test',primary:PRIMARY_TARGET,
  targets:Object.fromEntries(artifacts.map(({config})=>[config.id,config.manifestPath]))};
treeEntries.push({path:'targets.json',mode:'100644',type:'blob',content:JSON.stringify(targetIndex,null,2)+'\n'});
const tree=api('git/trees','POST',{...(base?{base_tree:base.tree.sha}:{}),tree:treeEntries});
const commitObject=api('git/commits','POST',{message:`Publish ${production?'production':'test'} build ${build}`,tree:tree.sha,parents:existing?[existing.commit.sha]:[]});
if(existing)api(`git/refs/heads/${branch}`,'PATCH',{sha:commitObject.sha,force:false});
else api('git/refs','POST',{ref:`refs/heads/${branch}`,sha:commitObject.sha});
setPublished(true);
console.log(`Published ${production?'production':'test'} build ${build} for ${artifacts.map(x=>x.config.id).join(', ')}`);
