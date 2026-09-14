'use strict';
const {replaceOnce,replaceFunctionBlock,readTemplate}=require('../../target-source.cjs');

// Only Chakshu uses NimBLE-Arduino. The shared C3/S3 source and core stay pinned.
function materializeBle(source) {
  let out=source;
  const replace=(before,after,label)=>out=replaceOnce(out,before,after,label);
  replace('#include <BLEDevice.h>','#include <NimBLEDevice.h>','Chakshu Bluetooth library');
  replace('#include <BLEServer.h>','','NimBLEDevice includes server');
  replace('#include <atomic>','#include <atomic>\nstd::atomic<uint16_t> chakshuConnectionHandle{BLE_HS_CONN_HANDLE_NONE};\nstd::atomic<bool> chakshuAudioSubscribed{false};','Chakshu connection ownership');
  out=replaceFunctionBlock(out,'class ServerCallbacks :','class ControlCallbacks :',readTemplate('xiao-sense','ble-server.cpp')+'\n','Chakshu server callbacks');
  out=replaceFunctionBlock(out,'class AudioCallbacks :','class DiagnosticsCallbacks :',readTemplate('xiao-sense','ble-audio.cpp')+'\n','Chakshu audio notifications');
  // NimBLE-Arduino invokes write callbacks synchronously with the current value.
  // Take one owned copy so an asynchronous status update cannot replace a command.
  replace('    OtaMessage message{};\n    const size_t size=characteristic->getLength();','    OtaMessage message{};\n    const auto written=characteristic->getValue();\n    const size_t size=written.size();','OTA command snapshot');
  replace('!characteristic->getData()','!written.data()','OTA command pointer');
  replace('memcpy(message.data,characteristic->getData(),size);','memcpy(message.data,written.data(),size);','OTA command bytes');
  replace('    const size_t length = characteristic->getLength();\n    const uint8_t* data = characteristic->getData();','    const auto written=characteristic->getValue();\n    const size_t length=written.size();\n    const uint8_t* data=written.data();','Audio command snapshot');
  replace('    const uint8_t* data=characteristic->getData();const size_t size=characteristic->getLength();','    const auto written=characteristic->getValue();\n    const uint8_t* data=written.data();const size_t size=written.size();','Recovery command snapshot');
  // No hand-managed CCCDs or Bluedroid-specific branches on this target.
  out=out.replace(/#if defined\(CONFIG_BLUEDROID_ENABLED\)\n[\s\S]*?#endif\n/g,'');
  out=out.replace(/\b(BLECharacteristicCallbacks|BLEServerCallbacks|BLECharacteristic|BLEService|BLEServer|BLEAdvertising|BLEDevice|BLEUUID)\b/g,'Nim$1');
  out=out.replace(/NimBLECharacteristic::PROPERTY_(READ|WRITE_NR|WRITE|NOTIFY)/g,'NIMBLE_PROPERTY::$1');
  out=out.replace(/void (onRead|onWrite)\(NimBLECharacteristic\* (\w+)\)/g,'void $1(NimBLECharacteristic* $2, NimBLEConnInfo&)');
  out=out.replace(/const String value=(\w+)->getValue\(\);/g,'const auto value=$1->getValue();');
  out=out.replace(/bleServer->getConnId\(\)/g,'chakshuConnectionHandle.load()');
  replace('bleServer->createService(NimBLEUUID(SERVICE_UUID),88)','bleServer->createService(SERVICE_UUID)','Native service handles');
  replace('  advertising->setScanResponse(true);\n  advertising->setMinPreferred(BLE_MIN_INTERVAL);\n  advertising->setMaxPreferred(BLE_MAX_INTERVAL);',
    '  advertising->enableScanResponse(true);\n  advertising->setName(DEVICE_NAME);\n  advertising->setPreferredParams(BLE_MIN_INTERVAL,BLE_MAX_INTERVAL);','Native advertising');
  replace('  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }',
    '  if (!chakshuAudioSubscribed.load()) { stopStreaming(ErrorCode::AUDIO_NOT_SUBSCRIBED);return; }\n  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }','Require actual audio subscription');
  replace('    audioCharacteristic->setValue(packet, AUDIO_HEADER_BYTES+length);','    // Send this immutable packet to the current subscribed connection.','Owned audio payload');
  replace('      const uint32_t rejectedBefore=notifyRejected.load();\n      // In the pinned Arduino BLE library, onStatus runs before notify returns.\n      // SUCCESS_NOTIFY means queued locally, not persisted by the phone.\n      audioCharacteristic->notify();\n      if(notifyRejected.load()==rejectedBefore) { accepted=true;break; }',
    '      if(sendChakshuAudio(packet,AUDIO_HEADER_BYTES+length)) { accepted=true;break; }','Native audio acceptance');
  // Initialize required state before advertising; model loading can take seconds.
  replace('  initializeBLE();\n  initializeRecovery();\n  ChakshuModel::initialize();\n  ChakshuVoice::initialize();',
    '  initializeRecovery();\n  ChakshuModel::initialize();\n  ChakshuVoice::initialize();','Finish model before advertising');
  replace('    fatalSetup("[FATAL] task allocation failed");\n  }\n}',
    '    fatalSetup("[FATAL] task allocation failed");\n  }\n  initializeBLE();\n}','Advertise after capture tasks are ready');
  if(/getData\(|getConnId\(|#include <BLE|\bBLECharacteristic\b/.test(out))throw Error('Unadapted Chakshu Bluetooth API');
  return out;
}
module.exports={materializeBle};
