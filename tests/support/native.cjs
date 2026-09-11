const fs=require('node:fs'),path=require('node:path'),os=require('node:os'),{execFileSync}=require('node:child_process');
function nativeTest(body,flags=[]){
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'synap-audio-'));
  try{
    const file=path.join(dir,'test.cpp'),binary=path.join(dir,'test');
    fs.writeFileSync(file,body);
    execFileSync('g++',['-std=c++17','-Wall','-Wextra','-Werror','-O2','-fsanitize=undefined','-fno-sanitize-recover=all',...flags,file,'-o',binary]);
    return execFileSync(binary,{encoding:'utf8',timeout:20000});
  }finally{fs.rmSync(dir,{recursive:true,force:true});}
}

module.exports={nativeTest};
