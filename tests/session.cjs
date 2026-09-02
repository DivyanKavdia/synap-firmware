// Compile the actual transport-independent firmware engine with a fake flash backend.
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path'),os=require('node:os'),{execFileSync}=require('node:child_process');
test('native firmware engine rejects mismatched/legacy targets before flash and preserves transfer recovery',()=>{
  const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
  const engine=source.split('// BEGIN EMBEDDED OtaSession.h')[1].split('// END EMBEDDED OtaSession.h')[0];
  const fixture=fs.readFileSync(path.join(__dirname,'session.cpp'),'utf8');
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'synap-ota-engine-'));
  try{
    fs.writeFileSync(path.join(dir,'session.cpp'),engine+'\n'+fixture);
    execFileSync('g++',['-std=c++17','-Wall','-Wextra','-Werror',path.join(dir,'session.cpp'),'-o',path.join(dir,'session')]);
    assert.match(execFileSync(path.join(dir,'session'),{encoding:'utf8'}),/PASS/);
  }finally{fs.rmSync(dir,{recursive:true,force:true});}
});
