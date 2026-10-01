'use strict';
const test=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path'),root=path.resolve(__dirname,'..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');
const list=p=>fs.existsSync(path.join(root,p))?fs.readdirSync(path.join(root,p)).filter(n=>/\.(cpp|h)$/.test(n)):[];

test('shared C++ modules are listed exactly once',()=>{
  const manifest=JSON.parse(read('firmware/shared/sources.json'));
  assert.equal(manifest.length,new Set(manifest).size);
  assert.deepEqual([...manifest].sort(),list('firmware/shared').sort());
  for(const n of ['odyssey-sd-detect.cpp','odyssey-sd-recording.cpp','odyssey-sd-transfer.cpp'])
    assert.ok(manifest.includes(n));
});
test('target C++ templates have live adapters',()=>{
  const owner={
    esp32c3:read('tools/boards/esp32c3/index.cjs'),
    'xiao-sense':read('tools/boards/xiao-sense/index.cjs')+'\n'+read('tools/boards/xiao-sense/ble.cjs')
  };
  for(const [board,code] of Object.entries(owner))
    assert.deepEqual(list('firmware/'+board).filter(n=>!code.includes("'"+n+"'")&&!code.includes('"'+n+'"')),[],'orphan target source: '+board);
  assert.doesNotMatch(owner.esp32c3,/discrete LED|\.\/led\.cjs/i);
});
test('board adapter files belong to the production tool graph',()=>{
  const visited=new Set(),queue=[path.join(root,'tools/materialize-target.cjs')];
  while(queue.length){
    const file=queue.pop();
    if(visited.has(file))continue;
    visited.add(file);
    for(const match of fs.readFileSync(file,'utf8').matchAll(/require\(['"](\.[^'"]+)['"]\)/g)){
      const base=path.resolve(path.dirname(file),match[1]);
      const target=fs.existsSync(base)&&fs.statSync(base).isFile()?base:base+'.cjs';
      if(fs.existsSync(target)&&!visited.has(target))queue.push(target);
    }
  }
  const orphaned=[];
  function walk(dir){
    for(const item of fs.readdirSync(dir,{withFileTypes:true})){
      const file=path.join(dir,item.name);
      if(item.isDirectory())walk(file);
      else if(item.isFile()&&item.name.endsWith('.cjs')&&!visited.has(file))orphaned.push(path.relative(root,file));
    }
  }
  walk(path.join(root,'tools/boards'));
  assert.deepEqual(orphaned,[],'unused board adapter');
});
