'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.join(__dirname,'..');
function markdownFiles(start){
  if(!fs.existsSync(start))return[];
  const stat=fs.statSync(start);
  if(stat.isFile())return /\.md$/i.test(start)?[start]:[];
  return fs.readdirSync(start,{withFileTypes:true}).flatMap(entry=>markdownFiles(path.join(start,entry.name)));
}
test('repository-relative documentation links resolve',()=>{
  const files=[path.join(root,'README.md'),...markdownFiles(path.join(root,'docs'))]
    .filter((value,index,list)=>list.indexOf(value)===index&&fs.existsSync(value));
  const broken=[];
  for(const file of files){
    const source=fs.readFileSync(file,'utf8');
    for(const match of source.matchAll(/\[[^\]]*\]\(([^)]+)\)/g)){
      let target=match[1].trim();
      if(!target||target.startsWith('#')||/^[a-z][a-z0-9+.-]*:/i.test(target))continue;
      target=target.split('#')[0].split('?')[0];
      if(!target)continue;
      try{target=decodeURIComponent(target)}catch(_){}
      const resolved=path.resolve(path.dirname(file),target);
      if(!fs.existsSync(resolved))broken.push(path.relative(root,file)+' -> '+target);
    }
  }
  assert.deepEqual(broken,[]);
});
