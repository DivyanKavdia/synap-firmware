'use strict';
const fs=require('node:fs'),path=require('node:path');

// Release preparation copies the reviewed sketch without rewriting firmware logic.
function prepareProduction(source){
  if(!source.includes('SYNAP-FW:esp32s3-fh4r2-qspi-4m:1.0.0:'))
    throw Error('Expected the primary S3 production sketch');
  return source;
}

if(require.main===module){
  const [input,output]=process.argv.slice(2);
  if(!input || !output)throw Error('Usage: node tools/prepare-production.cjs <source-sketch> <output-sketch>');
  const prepared=prepareProduction(fs.readFileSync(input,'utf8'));
  fs.mkdirSync(path.dirname(output),{recursive:true});
  fs.writeFileSync(output,prepared);
  console.log('Copied reviewed Synap production source');
}
module.exports={prepareProduction};
