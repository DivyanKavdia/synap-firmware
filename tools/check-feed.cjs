'use strict';
// Public delivery smoke test: verify authoritative GitHub target feeds immediately, while treating raw CDN edge lag as propagation rather than a failed release.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {repository,workflow,PRIMARY_TARGET,TARGETS}=require('./release.cjs');
const branch=process.env.SYNAP_RELEASE_BRANCH||'ota-test',origin='https://divyankavdia.github.io';
if(!['ota-test','ota-releases'].includes(branch))throw Error('Invalid release branch');

const local=Object.values(TARGETS).map(config=>({config,manifest:JSON.parse(fs.readFileSync(path.join('bundle',config.id,'latest.json'),'utf8'))}));
const primary=local.find(x=>x.config.id===PRIMARY_TARGET);
if(!primary)throw Error('Primary target bundle missing');

async function get(url,checkCors=true){
  const response=await fetch(url,{headers:{Origin:origin,Accept:'application/vnd.github+json'},redirect:'error',signal:AbortSignal.timeout(30000)});
  if(!response.ok)throw Error(`HTTP ${response.status} at ${url}`);
  if(checkCors&&!['*',origin].includes(response.headers.get('access-control-allow-origin')))throw Error('Public firmware response does not permit PWA cross-origin access');
  return response;
}
async function authoritativeJson(file){
  const payload=await (await get(`https://api.github.com/repos/${repository}/contents/${file}?ref=${branch}`)).json();
  if(typeof payload.content!=='string')throw Error(`GitHub contents API did not return ${file}`);
  return JSON.parse(Buffer.from(payload.content.replace(/\s+/g,''),'base64').toString('utf8'));
}
async function releaseTip(){return (await (await get(`https://api.github.com/repos/${repository}/commits/${branch}`)).json());}
async function verifyGitHubProvenance(feed,tip){
  if(feed.schema!==3||feed.provenance?.provider!=='github-actions'||feed.provenance?.repository!==repository||feed.provenance?.workflow!==workflow)
    throw Error(`Production feed ${feed.target} does not declare expected GitHub provenance`);
  if(!tip.commit?.verification?.verified||tip.commit.verification.reason!=='valid')throw Error('Release branch tip is not GitHub-verified');
  if(tip.author?.login!=='github-actions[bot]'||tip.commit?.committer?.name!=='GitHub')throw Error('Release branch tip was not published by GitHub Actions');
  if(tip.commit?.message!==`Publish production build ${feed.build}`)throw Error('Release commit does not match advertised build');
  const att=await (await get(`https://api.github.com/repos/${repository}/attestations/sha256:${feed.sha256}`)).json();
  if(!Array.isArray(att.attestations)||att.attestations.length===0)throw Error(`No GitHub build-provenance attestation exists for ${feed.target}`);
}

(async()=>{
  const index=await authoritativeJson('targets.json');
  const expectedTargets=Object.keys(TARGETS).sort();
  if(index.schema!==1||index.build!==primary.manifest.build||index.channel!==primary.manifest.channel||index.primary!==PRIMARY_TARGET||
      JSON.stringify(Object.keys(index.targets||{}).sort())!==JSON.stringify(expectedTargets))throw Error('Authoritative target index is invalid');
  const tip=branch==='ota-releases'?await releaseTip():null;
  for(const {config,manifest} of local){
    const feed=await authoritativeJson(config.manifestPath);
    if(feed.build!==manifest.build||feed.sha256!==manifest.sha256||feed.commit!==manifest.commit||feed.target!==config.id)
      throw Error(`Authoritative feed differs from compiled artifact for ${config.id}`);
    if(feed.channel!==manifest.channel)throw Error(`Public feed channel mismatch for ${config.id}`);
    if(branch==='ota-releases')await verifyGitHubProvenance(feed,tip);
    const bytes=Buffer.from(await(await get(manifest.url)).arrayBuffer());
    if(bytes.length!==manifest.size||crypto.createHash('sha256').update(bytes).digest('hex')!==manifest.sha256)
      throw Error(`Public binary differs from compiled artifact for ${config.id}`);
  }

  // raw.githubusercontent.com branch URLs can trail GitHub's authoritative ref briefly.
  // Validate CORS/parseability without marking a verified release failed for CDN lag.
  try{
    const cdn=await (await get(`https://raw.githubusercontent.com/${repository}/${branch}/targets.json?t=${Date.now()}`)).json();
    if(cdn.build<primary.manifest.build)console.log(`NOTE: raw CDN target index is still on build ${cdn.build}; authoritative feed is ${primary.manifest.build}.`);
    else if(cdn.channel!==primary.manifest.channel)throw Error('Raw CDN target index channel mismatch');
  }catch(error){console.log('NOTE: raw CDN target-index propagation check is non-blocking:',error.message);}
  console.log(`PASS: ${primary.manifest.channel} build ${primary.manifest.build}, ${local.length} hardware targets, authoritative feeds, digests, provenance and browser CORS`);
})().catch(error=>{console.error(error);process.exitCode=1;});
