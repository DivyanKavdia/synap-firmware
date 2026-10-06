'use strict';
// Arduino ESP32 3.3.5 accepts some invalid write-data responses when a later
// CMD13 succeeds. Require the actual data-accepted token on C3 only.
const fs=require('node:fs'),path=require('node:path');
function patch(source){
 if(source.includes('// SYNAP_C3_SD_WRITE_ACCEPTED'))return source;
 const before=`      } else if (token == 0x0C) {
        return false;
      }

      unsigned int resp;`;
 if(source.split(before).length!==2)throw Error('Pinned SD write acceptance shape changed');
 return source.replace(before,`      } else if (token == 0x0C) {
        return false;
      }
#if CONFIG_IDF_TARGET_ESP32C3
      // SYNAP_C3_SD_WRITE_ACCEPTED
      if (token != 0x05) return false;
#endif

      unsigned int resp;`);
}
if(require.main===module){
 const file=path.join(process.argv[2],'sd_diskio.cpp');
 fs.writeFileSync(file,patch(fs.readFileSync(file,'utf8')));
}
module.exports={patch};
