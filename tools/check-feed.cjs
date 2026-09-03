'use strict';
// Public delivery smoke test: same cross-origin trust path used by synap-pwa.
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
  for(let attempt=0;attempt<6;attempt++){
    try{
      const feed=await (await get(`https://raw.githubusercontent.com/${repository}/${branch}/latest.json?t=${Date.now()}`)).json();
      if(feed.build<manifest.build)throw Error('CDN still serves an older manifest');
      if(feed.channel!==manifest.channel)throw Error('Public feed channel mismatch');
      if(branch==='ota-releases')await verifyGitHubProvenance(feed);
      const bytes=Buffer.from(await(await get(manifest.url)).arrayBuffer());
      if(bytes.length!==manifest.size||crypto.createHash('sha256').update(bytes).digest('hex')!==manifest.sha256)throw Error('Public binary differs from compiled artifact');
      console.log(`PASS: ${manifest.channel} build ${manifest.build}, digest, GitHub provenance and browser CORS`);return;
    }catch(error){if(attempt===5)throw error;console.log('Waiting for GitHub CDN/provenance:',error.message);await new Promise(resolve=>setTimeout(resolve,10000));}
  }
})().catch(error=>{console.error(error);process.exitCode=1;});
