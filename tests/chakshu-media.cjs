'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
test('SD operations reject stale requests and competing audio/OTA without losing accepted results',()=>{
  const source=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/media.cpp'),'utf8');
  const start=source.indexOf('void tick() {'),end=source.indexOf('\n}\n}\nbool mediaBusy()',start)+2;
  assert(start>=0&&end>start);
  const fixture=fs.readFileSync(path.join(__dirname,'chakshu-media.cpp'),'utf8');
  assert.match(nativeTest(fixture.replace('// INSERT MEDIA TICK',source.slice(start,end))),/PASS media ownership/);
});
