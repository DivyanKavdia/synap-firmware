'use strict';

const PRIMARY_TARGET='esp32s3-fh4r2-qspi-4m';
const TARGETS=Object.freeze({
  [PRIMARY_TARGET]:Object.freeze({
    id:PRIMARY_TARGET,
    family:'esp32s3',
    board:'ESP32-S3 SuperMini',
    chip:9,
    flashBytes:4194304,
    psramBytes:2097152,
    partition:'default',
    slotSize:0x140000,
    productMarker:'SYNAP-ESP32S3-OTA-ID-V3',
    manifestPath:'latest.json',
    releasePrefix:'',
    sourceName:'synap_esp32s3.ino'
  }),
  'esp32c3-supermini-4m':Object.freeze({
    id:'esp32c3-supermini-4m',
    family:'esp32c3',
    board:'ESP32-C3 SuperMini',
    chip:5,
    flashBytes:4194304,
    psramBytes:0,
    partition:'default',
    slotSize:0x140000,
    productMarker:'SYNAP-ESP32C3-OTA-ID-V3',
    manifestPath:'targets/esp32c3-supermini-4m/latest.json',
    releasePrefix:'targets/esp32c3-supermini-4m/',
    sourceName:'synap_esp32c3.ino'
  })
});

function getTarget(id){
  const target=TARGETS[id];
  if(!target)throw Error(`Unknown firmware target: ${id}`);
  return target;
}

module.exports={PRIMARY_TARGET,TARGETS,getTarget};
