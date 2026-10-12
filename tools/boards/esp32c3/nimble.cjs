'use strict';
// C3-only switch to the already pinned NimBLE-Arduino 2.3.6 stack.
// Does not alter the S3/Chakshu materializers or the deployed partition table.
const {replaceOnce,replaceFunctionBlock,readTemplate}=require('../../target-source.cjs');
function materializeC3Nimble(source) {
  let out=source;
  const change=(before,after,label)=>out=replaceOnce(out,before,after,label);
  change('#include <BLEDevice.h>','#include <NimBLEDevice.h>','C3 native NimBLE');
  change('#include <BLEServer.h>','','C3 native BLE server');
  change('#include <atomic>','#include <atomic>\nstd::atomic<uint16_t> c3BleHandle{BLE_HS_CONN_HANDLE_NONE};\nstd::atomic<bool> c3AudioSubscribed{false};','Owned C3 connection');
  out=out.replace(/#if defined\(CONFIG_BLUEDROID_ENABLED\)\n[\s\S]*?#endif\n/g,'');
  out=replaceFunctionBlock(out,'class ServerCallbacks :','class AudioCallbacks :',readTemplate('esp32c3','ble-server.cpp')+'\n','C3 native link and control');
  out=replaceFunctionBlock(out,'class AudioCallbacks :','class DiagnosticsCallbacks :',readTemplate('esp32c3','ble-audio.cpp')+'\n','C3 owned PCM audio');
  out=out.replace(/\b(BLECharacteristicCallbacks|BLEServerCallbacks|BLECharacteristic|BLEService|BLEServer|BLEAdvertising|BLEDevice|BLEUUID)\b/g,'Nim$1');
  out=out.replace(/NimBLECharacteristic::PROPERTY_(READ|WRITE_NR|WRITE|NOTIFY)/g,'NIMBLE_PROPERTY::$1');
  out=out.replace(/void (onRead|onWrite)\(NimBLECharacteristic\* (\w+)\)/g,'void $1(NimBLECharacteristic* $2, NimBLEConnInfo&)');
  out=out.replace(/bleServer->getConnId\(\)/g,'c3BleHandle.load()');
  change('bleServer->createService(NimBLEUUID(SERVICE_UUID),64)','bleServer->createService(SERVICE_UUID)','C3 native service allocation');
  change('#if defined(CONFIG_NIMBLE_ENABLED)\n  bleServer->advertiseOnDisconnect(true);\n#endif',
    '  bleServer->advertiseOnDisconnect(true);','C3 native re-advertising');
  change('controlCharacteristic=service->createCharacteristic(CONTROL_CHAR_UUID,\n    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |\n    NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);\n  controlCharacteristic->setCallbacks(new ControlCallbacks());',
    'controlCharacteristic=new C3ControlCharacteristic();\n  service->addCharacteristic(controlCharacteristic);','C3 owned command/status characteristic');
  change('deviceIdentity->setValue(synapDeviceId);',
    'deviceIdentity->setValue(reinterpret_cast<const uint8_t*>(synapDeviceId),strlen(synapDeviceId));','Exact C3 identity length');
  change('identity->setValue(SYNAP_FIRMWARE_ID);',
    'identity->setValue(reinterpret_cast<const uint8_t*>(SYNAP_FIRMWARE_ID),sizeof(SYNAP_FIRMWARE_ID)-1);','Exact C3 firmware identity length');
  change('  advertising->setScanResponse(true);\n  advertising->setMinPreferred(BLE_MIN_INTERVAL);\n  advertising->setMaxPreferred(BLE_MAX_INTERVAL);',
    '  advertising->enableScanResponse(true);\n  advertising->setName(DEVICE_NAME);\n  advertising->setPreferredParams(BLE_MIN_INTERVAL,BLE_MAX_INTERVAL);\n  advertising->setMinInterval(48);\n  advertising->setMaxInterval(48);','C3 native advertising policy');
  change('    OtaMessage message{};\n    const size_t size=characteristic->getLength();',
    '    OtaMessage message{};\n    const auto written=characteristic->getValue();\n    const size_t size=written.size();','C3 OTA write snapshot');
  change('!characteristic->getData()','!written.data()','C3 OTA buffer guard');
  change('memcpy(message.data,characteristic->getData(),size);','memcpy(message.data,written.data(),size);','C3 OTA owned payload');
  change('    const uint8_t* data=characteristic->getData();const size_t size=characteristic->getLength();',
    '    const auto written=characteristic->getValue();\n    const uint8_t* data=written.data();const size_t size=written.size();','C3 recovery request snapshot');
  change('    const size_t length=characteristic->getLength();\n    const uint8_t* p=characteristic->getData();',
    '    const auto written=characteristic->getValue();\n    const size_t length=written.size();\n    const uint8_t* p=written.data();','C3 SD media request snapshot');
  change('    audioCharacteristic->setValue(packet, AUDIO_HEADER_BYTES+length);',
    '    // Native NimBLE sends an owned PCM packet to the subscribed handle.','C3 audio packet ownership');
  change('      const uint32_t rejectedBefore=notifyRejected.load();\n      // In the pinned Arduino BLE library, onStatus runs before notify returns.\n      // SUCCESS_NOTIFY means queued locally, not persisted by the phone.\n      audioCharacteristic->notify();\n      if(notifyRejected.load()==rejectedBefore) { accepted=true;break; }',
    '      if(c3SendAudio(packet,AUDIO_HEADER_BYTES+length)) { accepted=true;break; }','C3 audio admission');
  change('  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }',
    '  if (!c3AudioSubscribed.load()) { stopStreaming(ErrorCode::AUDIO_NOT_SUBSCRIBED);return; }\n  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }','C3 require CCCD subscription');
  if(/#include <BLE|\bBLECharacteristic\b|\bBLEServer\b|->getData\(|->getLength\(|->getConnId\(|new ControlCallbacks\(/.test(out))
    throw Error('Unadapted C3 native NimBLE API');
  return out;
}
module.exports={materializeC3Nimble};
