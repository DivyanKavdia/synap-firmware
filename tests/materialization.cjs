'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path'),os=require('node:os');
const {execFileSync}=require('node:child_process');
const {materialize}=require('../tools/materialize-target.cjs');
const {replaceOnce,replaceFunctionBlock}=require('../tools/target-source.cjs');
const root=path.resolve(__dirname,'..');
const input=path.join(root,'synap_esp32s3/synap_esp32s3.ino');
const source=fs.readFileSync(input,'utf8');

test('target generation resolves feature templates outside the repository cwd',()=>{
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'synap-target-'));
  try{
    for(const target of ['esp32s3-fh4r2-qspi-4m','esp32c3-supermini-4m']){
      const output=path.join(dir,target,'firmware.ino');
      execFileSync(process.execPath,[path.join(root,'tools/materialize-target.cjs'),target,input,output],{cwd:dir});
      assert.equal(fs.readFileSync(output,'utf8'),materialize(source,target));
    }
    assert.equal(materialize(source,'esp32s3-fh4r2-qspi-4m'),source);
  }finally{fs.rmSync(dir,{recursive:true,force:true});}
});

test('target edits fail closed when shared anchors change or become ambiguous',()=>{
  assert.throws(()=>replaceOnce('a','b','c','pin'),/Missing.*pin/);
  assert.throws(()=>replaceOnce('aa','a','c','pin'),/Ambiguous.*pin/);
  assert.throws(()=>replaceFunctionBlock('begin','begin','end','new','function'),/Missing.*end/);
  assert.throws(()=>replaceFunctionBlock('begin begin end','begin','end','new','function'),/Ambiguous/);
  assert.throws(()=>materialize(source.replace('#define SYNAP_TOUCH_PIN 13',''), 'esp32c3-supermini-4m'),/Missing.*touch/);
  assert.throws(()=>materialize(source,'unknown-board'),/Unknown firmware target/);
});
