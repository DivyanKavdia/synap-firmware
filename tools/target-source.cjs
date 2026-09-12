'use strict';
const fs=require('node:fs'),path=require('node:path');

function readTemplate(board,name){
  return fs.readFileSync(path.join(__dirname,'..','firmware',board,name),'utf8');
}

function replaceOnce(source,before,after,label){
  const first=source.indexOf(before);
  if(first<0)throw Error(`Missing target materialization anchor: ${label}`);
  if(source.indexOf(before,first+before.length)>=0)throw Error(`Ambiguous target materialization anchor: ${label}`);
  return source.slice(0,first)+after+source.slice(first+before.length);
}

function replaceFunctionBlock(source,startMarker,endMarker,replacement,label){
  const start=source.indexOf(startMarker);
  if(start<0)throw Error(`Missing target materialization function: ${label}`);
  const end=source.indexOf(endMarker,start);
  if(end<0)throw Error(`Missing target materialization function end: ${label}`);
  if(source.indexOf(startMarker,start+startMarker.length)>=0)throw Error(`Ambiguous target materialization function: ${label}`);
  return source.slice(0,start)+replacement+source.slice(end);
}

module.exports={replaceOnce,replaceFunctionBlock,readTemplate};
