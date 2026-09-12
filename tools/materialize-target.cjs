'use strict';
const fs=require('node:fs'),path=require('node:path');
const {PRIMARY_TARGET,getTarget}=require('./targets.cjs');
const {materializeC3}=require('./boards/esp32c3/index.cjs');

function materialize(source,targetId){
  const target=getTarget(targetId);
  if(target.id===PRIMARY_TARGET)return source;
  if(target.family==='esp32c3')return materializeC3(source,target);
  throw Error(`No materializer for ${target.id}`);
}

if(require.main===module){
  const args=process.argv.slice(2),check=args[0]==='--check';
  if(check)args.shift();
  const [targetId,input,output]=args;
  if(!targetId||!input)throw Error('Usage: node tools/materialize-target.cjs [--check] <target> <prepared-source> [output]');
  const source=fs.readFileSync(input,'utf8'),result=materialize(source,targetId);
  if(check){console.log(`PASS: materialized ${targetId}`);process.exit(0);}
  if(!output)throw Error('Output path is required unless --check is used');
  fs.mkdirSync(path.dirname(output),{recursive:true});
  fs.writeFileSync(output,result);
  console.log(`Materialized ${targetId} -> ${output}`);
}

module.exports={materialize};
