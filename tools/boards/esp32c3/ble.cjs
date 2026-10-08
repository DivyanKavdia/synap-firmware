'use strict';
const {replaceOnce,replaceFunctionBlock}=require('../../target-source.cjs');

// Odyssey C3 validation adapter: preserve the existing Synap GATT protocol
// while replacing Arduino-Bluedroid with NimBLE-Arduino. This is target-local;
// Odyssey S3 remains on the pinned Arduino BLE implementation.
function materializeC3Ble(source){
  let out=source;
  const replace=(before,after,label)=>out=replaceOnce(out,before,after,label);

  replace('#include <BLEDevice.h>','#include <NimBLEDevice.h>','C3 NimBLE include');
  replace('#include <BLEServer.h>','','C3 NimBLE server include');
  replace('#include <atomic>','#include <atomic>\nstd::atomic<uint16_t> odysseyConnectionHandle{BLE_HS_CONN_HANDLE_NONE};\nstd::atomic<bool> odysseyAudioSubscribed{false};','C3 NimBLE connection ownership');

  // Remove Bluedroid-only descriptors and callback overloads. NimBLE creates
  // CCCDs for NOTIFY characteristics automatically.
  out=out.replace(/#if defined\(CONFIG_BLUEDROID_ENABLED\)\n[\s\S]*?#endif\n/g,'');

  const server=[
    'class ServerCallbacks : public BLEServerCallbacks {',
    '  void onConnect(BLEServer* server, BLEConnInfo& peer) override {',
    '    if(deviceConnected.load()){server->disconnect(peer.getConnHandle());return;}',
    '    odysseyConnectionHandle=peer.getConnHandle();odysseyAudioSubscribed=false;',
    '    ++connectionGeneration;',
    '    if(!recoveryWaiting.load())streamingEnabled.store(false);',
    '    deviceConnected.store(true);connectedLedAt=millis();connectionEventPending.store(true);',
    '    server->updateConnParams(peer.getConnHandle(),BLE_MIN_INTERVAL,BLE_MAX_INTERVAL,',
    '      BLE_SLAVE_LATENCY,BLE_SUPERVISION_TIMEOUT);',
    '  }',
    '  void onDisconnect(BLEServer*, BLEConnInfo& peer, int reason) override {',
    '    if(peer.getConnHandle()!=odysseyConnectionHandle.load())return;',
    '    odysseyConnectionHandle=BLE_HS_CONN_HANDLE_NONE;odysseyAudioSubscribed=false;',
    '    deviceConnected.store(false);',
    '    ++linkDisconnects;lastDisconnectAt=millis();lastDisconnectReason=uint16_t(reason);',
    '    if(recoveryEnabled.load()&&streamingEnabled.load()){',
    '      recoveryWaiting=true;if(!recoveryWaitingAt.load())recoveryWaitingAt=millis();',
    '    }else streamingEnabled.store(false);',
    '    ++connectionGeneration;connectionEventPending.store(true);',
    '  }',
    '};',
    ''
  ].join('\n');
  out=replaceFunctionBlock(out,'class ServerCallbacks :','class ControlCallbacks :',server,'C3 NimBLE server callbacks');

  const control=[
    'class ControlCallbacks : public BLECharacteristicCallbacks {',
    '  void onRead(BLECharacteristic*, BLEConnInfo&) override { updateStatusCharacteristic(false); }',
    '  void onWrite(BLECharacteristic* characteristic, BLEConnInfo& peer) override {',
    '    if(peer.getConnHandle()!=odysseyConnectionHandle.load())return;',
    '    const auto written=characteristic->getValue();',
    '    const size_t length=written.size();const uint8_t* data=written.data();',
    '    const uint8_t command=length==2&&data?data[0]:0xFF;',
    '    const uint8_t version=length==2&&data?data[1]:0;',
    '    queueEvent(EventType::COMMAND,command,version,streamGeneration.load());',
    '  }',
    '};',
    ''
  ].join('\n');
  out=replaceFunctionBlock(out,'class ControlCallbacks :','class AudioCallbacks :',control,'C3 NimBLE control callbacks');

  const audio=[
    'class AudioCallbacks : public BLECharacteristicCallbacks {',
    '  void onSubscribe(BLECharacteristic*, BLEConnInfo& peer, uint16_t flags) override {',
    '    if(peer.getConnHandle()==odysseyConnectionHandle.load())odysseyAudioSubscribed=(flags&1)!=0;',
    '  }',
    '  void onStatus(BLECharacteristic*, int code) override {',
    '    if(code){lastNotifyStatus=uint16_t(code);lastNotifyError=uint32_t(code);++notifyRejected;}',
    '  }',
    '};',
    ''
  ].join('\n');
  out=replaceFunctionBlock(out,'class AudioCallbacks :','class DiagnosticsCallbacks :',audio,'C3 NimBLE audio callbacks');

  // All other value callbacks use NimBLE peer-aware signatures.
  out=out.replace(/void onRead\(BLECharacteristic\* (\w+)\) override/g,'void onRead(BLECharacteristic* $1, BLEConnInfo&) override');
  out=out.replace(/void onRead\(BLECharacteristic\*\) override/g,'void onRead(BLECharacteristic*, BLEConnInfo&) override');
  out=out.replace(/void onWrite\(BLECharacteristic\* (\w+)\) override/g,'void onWrite(BLECharacteristic* $1, BLEConnInfo&) override');

  // Snapshot NimBLE characteristic values before queueing work.
  out=out.replace(
    /const size_t size=characteristic->getLength\(\);\n    if \(!size \|\| size>sizeof\(message\.data\) \|\| !characteristic->getData\(\)\) return;\n    message\.connection=connectionGeneration\.load\(\);message\.length=size;\n    memcpy\(message\.data,characteristic->getData\(\),size\);/g,
    'const auto written=characteristic->getValue();\n    const size_t size=written.size();\n    if (!size || size>sizeof(message.data) || !written.data()) return;\n    message.connection=connectionGeneration.load();message.length=size;\n    memcpy(message.data,written.data(),size);');
  out=out.replace(
    /const uint8_t\* data=characteristic->getData\(\);const size_t size=characteristic->getLength\(\);/g,
    'const auto written=characteristic->getValue();\n    const uint8_t* data=written.data();const size_t size=written.size();');
  out=out.replace(
    /const size_t length=characteristic->getLength\(\);\n    const uint8_t\* p=characteristic->getData\(\);/g,
    'const auto written=characteristic->getValue();\n    const size_t length=written.size();\n    const uint8_t* p=written.data();');

  out=out.replace(/BLECharacteristic::PROPERTY_(READ|WRITE_NR|WRITE|NOTIFY)/g,'NIMBLE_PROPERTY::$1');
  replace('bleServer->createService(BLEUUID(SERVICE_UUID),64)','bleServer->createService(SERVICE_UUID)','C3 NimBLE service creation');
  replace('deviceIdentity->setValue(synapDeviceId);','deviceIdentity->setValue(reinterpret_cast<const uint8_t*>(synapDeviceId),strlen(synapDeviceId));','C3 device ID length');
  replace('identity->setValue(SYNAP_FIRMWARE_ID);','identity->setValue(reinterpret_cast<const uint8_t*>(SYNAP_FIRMWARE_ID),sizeof(SYNAP_FIRMWARE_ID)-1);','C3 firmware identity length');
  out=out.replace(/bleServer->getPeerMTU\(bleServer->getConnId\(\)\)/g,'bleServer->getPeerMTU(odysseyConnectionHandle.load())');

  const oldNotify=[
    '      const uint32_t rejectedBefore=notifyRejected.load();',
    '      // In the pinned Arduino BLE library, onStatus runs before notify returns.',
    '      // SUCCESS_NOTIFY means queued locally, not persisted by the phone.',
    '      audioCharacteristic->notify();',
    '      if(notifyRejected.load()==rejectedBefore) { accepted=true;break; }'
  ].join('\n');
  const newNotify=[
    '      if(audioCharacteristic->notify(packet,AUDIO_HEADER_BYTES+length,odysseyConnectionHandle.load())) { accepted=true;break; }',
    '      ++notifyRejected;'
  ].join('\n');
  replace(oldNotify,newNotify,'C3 NimBLE audio notify ownership');

  // Preserve the current subscription contract explicitly under NimBLE.
  const startGate='  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }';
  replace(startGate,'  if (!odysseyAudioSubscribed.load()) { stopStreaming(ErrorCode::AUDIO_NOT_SUBSCRIBED); return; }\n'+startGate,'C3 NimBLE START subscription gate');

  replace('  advertising->setScanResponse(true);\n  advertising->setMinPreferred(BLE_MIN_INTERVAL);\n  advertising->setMaxPreferred(BLE_MAX_INTERVAL);',
    '  advertising->enableScanResponse(true);\n  advertising->setName(DEVICE_NAME);\n  advertising->setPreferredParams(BLE_MIN_INTERVAL,BLE_MAX_INTERVAL);\n  advertising->setMinInterval(32);\n  advertising->setMaxInterval(32);',
    'C3 NimBLE advertising');
  replace('#if defined(CONFIG_NIMBLE_ENABLED)\n  bleServer->advertiseOnDisconnect(true);\n#endif','  bleServer->advertiseOnDisconnect(true);','C3 NimBLE reconnect advertising');

  if(/#include <BLE|BLE2902|getData\(\)|getConnId\(\)|BLECharacteristic::PROPERTY_/.test(out))
    throw Error('Unadapted C3 Bluetooth API');
  return out;
}

module.exports={materializeC3Ble};
