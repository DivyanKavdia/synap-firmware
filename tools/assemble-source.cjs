'use strict';
const fs=require('node:fs'),path=require('node:path');
const {renderProfile}=require('./device-profile.cjs');
const {PRIMARY_TARGET,getTarget}=require('./targets.cjs');
const root=path.resolve(__dirname,'..'),dir=path.join(root,'firmware/shared');
function assemble() {
  const files=JSON.parse(fs.readFileSync(path.join(dir,'sources.json'),'utf8'));
  return files.map(file=>fs.readFileSync(path.join(dir,file),'utf8')).join('').replace('// SYNAP_DEVICE_PROFILE',renderProfile(getTarget(PRIMARY_TARGET)));
}
if(require.main===module) {
  const output=path.join(root,'synap_esp32s3/synap_esp32s3.ino'),source=assemble();
  if(process.argv.includes('--check')) {
    if(fs.readFileSync(output,'utf8')!==source)throw Error('Regenerate the Arduino source: node tools/assemble-source.cjs');
  } else fs.writeFileSync(output,source);
}
module.exports={assemble};
