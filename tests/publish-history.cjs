'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const source=fs.readFileSync(path.join(__dirname,'../tools/publish.cjs'),'utf8');
const api=source.slice(source.indexOf('function api('),source.indexOf('function setPublished('));
const release=source.slice(source.indexOf('if(production){'),source.indexOf('const treeEntries='));

function publish({existing=false,lookupError=false}={}) {
  const version='synap-os1-build1205',calls=[];
  const history=Array.from({length:100},(_,i)=>({tag_name:existing&&i===0?version:'synap-os1-build'+(1200-i),
    assets:[{metadata:'x'.repeat(16000)}]}));
  assert(Buffer.byteLength(JSON.stringify(history))>1024*1024,'fixture exceeds the default subprocess limit');
  const context={repo:'example/firmware',version,commit:'a'.repeat(40),production:true,path,
    fs:{mkdirSync(){},copyFileSync(){}},
    artifacts:['esp32s3','esp32c3'].map(family=>({config:{family,sourceName:'synap_'+family+'.ino'},dir:'bundle/'+family})),
    execFileSync(command,args,options){
      calls.push(args);
      assert.equal(command,'gh');
      if(args[0]==='release')return '';
      assert.equal(args[1],'repos/example/firmware/releases?per_page=100');
      if(lookupError)throw Error('GitHub release lookup failed');
      const at=args.indexOf('--jq');
      if(at>=0)assert.equal(args[at+1],'[.[].tag_name]');
      const output=JSON.stringify(at>=0?history.map(r=>r.tag_name):history);
      if(Buffer.byteLength(output)>(options.maxBuffer||1024*1024))throw Error('spawnSync gh ENOBUFS');
      return output;
    }};
  vm.runInNewContext(api+release,context);
  return calls;
}

test('large release histories publish both target assets without overflowing subprocess output',()=>{
  const calls=publish(),create=calls.find(args=>args[0]==='release');
  assert(create,'a missing tag must be created');
  for(const family of ['esp32s3','esp32c3'])assert(create.includes('bundle/release-assets/firmware-'+family+'.bin'));
  assert.equal(create[create.indexOf('--target')+1],'a'.repeat(40));
});

test('an existing release remains idempotent and lookup failures cannot create a release',()=>{
  assert.equal(publish({existing:true}).filter(args=>args[0]==='release').length,0);
  assert.throws(()=>publish({lookupError:true}),/GitHub release lookup failed/);
});
