'use strict';
// Arduino ESP32 3.3.5 accepts some invalid single-sector write-data responses
// when a later CMD13 succeeds. Also, FatFS can coalesce filesystem flushes and
// call ff_sd_write(count>1), which enters CMD25 even if the application itself
// writes only 512-byte chunks. On Odyssey C3 force those requests through
// repeated CMD24 single-sector writes so no write path can enter CMD25.
const fs=require('node:fs'),path=require('node:path');
function replaceOnce(source,before,after,label){
 if(source.split(before).length!==2)throw Error('Pinned SD write shape changed: '+label);
 return source.replace(before,after);
}
function patch(source){
 if(!source.includes('// SYNAP_C3_SD_WRITE_ACCEPTED')){
  const before=`      char token = sdWriteBytes(pdrv, buffer, 0xFE);
      sdDeselectCard(pdrv);

      if (token == 0x0A) {
        continue;
      } else if (token == 0x0C) {
        return false;
      }

      unsigned int resp;`;
  source=replaceOnce(source,before,`      char token = sdWriteBytes(pdrv, buffer, 0xFE);
#if CONFIG_IDF_TARGET_ESP32C3
      // SYNAP_C3_SD_WRITE_ACCEPTED
      // A CMD24 write is not complete when the data-response token is accepted.
      // Keep CS asserted and provide clocks until the card exits its busy
      // programming interval, matching ESP-IDF SDSPI's write transaction order.
      if (token != 0x05) {
        sdDeselectCard(pdrv);
        return false;
      }
      if (!sdWait(pdrv, 1000)) {
        sdDeselectCard(pdrv);
        return false;
      }
      sdDeselectCard(pdrv);
#else
      sdDeselectCard(pdrv);

      if (token == 0x0A) {
        continue;
      } else if (token == 0x0C) {
        return false;
      }
#endif

      unsigned int resp;`,'single-sector completion');
 }
 if(!source.includes('// SYNAP_C3_SD_SINGLE_SECTOR_ONLY')){
  const before=`  if (count > 1) {
    res = sdWriteSectors(pdrv, (const char *)buffer, sector, count) ? RES_OK : RES_ERROR;
  } else {
    res = sdWriteSector(pdrv, (const char *)buffer, sector) ? RES_OK : RES_ERROR;
  }`;
  source=replaceOnce(source,before,`#if CONFIG_IDF_TARGET_ESP32C3
  // SYNAP_C3_SD_SINGLE_SECTOR_ONLY
  // FatFS may batch dirty data/FAT/directory sectors during f_sync(). Keep the
  // physical card protocol on CMD24 even when count > 1 so a failed sync cannot
  // strand the still-powered card inside a CMD25 multi-block write transaction.
  for (UINT i=0; i<count; ++i) {
    if (!sdWriteSector(pdrv, (const char *)buffer + (size_t(i) << 9), sector + i)) {
      res = RES_ERROR;
      break;
    }
  }
#else
  if (count > 1) {
    res = sdWriteSectors(pdrv, (const char *)buffer, sector, count) ? RES_OK : RES_ERROR;
  } else {
    res = sdWriteSector(pdrv, (const char *)buffer, sector) ? RES_OK : RES_ERROR;
  }
#endif`,'FatFS multi-sector write');
 }
 return source;
}
if(require.main===module){
 const file=path.join(process.argv[2],'sd_diskio.cpp');
 fs.writeFileSync(file,patch(fs.readFileSync(file,'utf8')));
}
module.exports={patch};
