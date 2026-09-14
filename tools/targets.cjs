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
  }),
  'xiao-esp32s3-sense-8m':Object.freeze({
    id:'xiao-esp32s3-sense-8m',family:'esp32s3',assetStem:'chakshu',board:'Chakshu (XIAO ESP32S3 Sense)',
    chip:9,flashBytes:8388608,psramBytes:8388608,partition:'default_8MB',slotSize:0x330000,
    productMarker:'SYNAP-CHAKSHU-OTA-ID-V3',
    manifestPath:'targets/xiao-esp32s3-sense-8m/latest.json',releasePrefix:'targets/xiao-esp32s3-sense-8m/',
    sourceName:'synap_chakshu.ino'
  })
});

function getTarget(id){
  const target=TARGETS[id];
  if(!target)throw Error(`Unknown firmware target: ${id}`);
  return target;
}

module.exports={PRIMARY_TARGET,TARGETS,getTarget};
