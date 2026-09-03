'use strict';
// Public delivery smoke test: same cross-origin URLs used by synap-pwa.
const fs=require('node:fs'),crypto=require('node:crypto'),{canonicalManifest}=require('./release.cjs');
const manifest=JSON.parse(fs.readFileSync('bundle/latest.json','utf8'));
const branch=process.env.SYNAP_RELEASE_BRANCH||'ota-test',origin='https://divyankavdia.github.io';
if(!['ota-test','ota-releases'].includes(branch))throw Error('Invalid release branch');
if(branch==='ota-releases') {
  const publicKey=fs.readFileSync('tools/release-public-key.pem','utf8');
  if(!manifest.signing)throw Error('Production manifest missing signature');
  const ok=crypto.verify('sha256',Buffer.from(canonicalManifest(manifest)),{key:publicKey,dsaEncoding:'ieee-p1363'},Buffer.from(manifest.signing.value,'base64'));
  if(!ok)throw Error('Production manifest signature verification failed');
}
async function get(url){
  const response=await fetch(url,{headers:{Origin:origin},redirect:'error',signal:AbortSignal.timeout(30000)});
  if(!response.ok)throw Error(`HTTP ${response.status} at ${url}`);
  if(!['*',origin].includes(response.headers.get('access-control-allow-origin')))throw Error('Public firmware response does not permit PWA cross-origin access');
  return response;
}
(async()=>{
  for(let attempt=0;attempt<6;attempt++){
    try{
      const feed=await (await get(`https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/${branch}/latest.json?t=${Date.now()}`)).json();
      if(feed.build<manifest.build)throw Error('CDN still serves an older manifest');
      if(feed.channel!==manifest.channel)throw Error('Public feed channel mismatch');
      const bytes=Buffer.from(await(await get(manifest.url)).arrayBuffer());
      if(bytes.length!==manifest.size||crypto.createHash('sha256').update(bytes).digest('hex')!==manifest.sha256)throw Error('Public binary differs from compiled artifact');
      console.log(`PASS: ${manifest.channel} manifest and build ${manifest.build} binary, digest, signature policy and browser CORS`);return;
    }catch(error){if(attempt===5)throw error;console.log('Waiting for GitHub CDN:',error.message);await new Promise(resolve=>setTimeout(resolve,10000));}
  }
})().catch(error=>{console.error(error);process.exitCode=1;});
