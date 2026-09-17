'use strict';
const {replaceOnce,replaceFunctionBlock,readTemplate}=require('../../target-source.cjs');

// Only Chakshu uses NimBLE-Arduino. The shared C3/S3 source and core stay pinned.
function materializeBle(source) {
  let out=source;
  const replace=(before,after,label)=>out=replaceOnce(out,before,after,label);
  replace('#include <BLEDevice.h>','#include <NimBLEDevice.h>','Chakshu Bluetooth library');
  replace('#include <BLEServer.h>','','NimBLEDevice includes server');
  // NimBLE's generic overload serializes mutable char arrays with sizeof(T),
  // including the NUL and unused capacity. GATT text is length-delimited.
  replace('deviceIdentity->setValue(synapDeviceId);',
    'deviceIdentity->setValue(reinterpret_cast<const uint8_t*>(synapDeviceId),strlen(synapDeviceId));','Device ID text bytes');
  replace('identity->setValue(SYNAP_FIRMWARE_ID);',
    'identity->setValue(reinterpret_cast<const uint8_t*>(SYNAP_FIRMWARE_ID),sizeof(SYNAP_FIRMWARE_ID)-1);','Firmware identity text bytes');
  replace('characteristic->setValue(s.path);',
    'characteristic->setValue(reinterpret_cast<const uint8_t*>(s.path),strlen(s.path));','SD path text bytes');
  replace('#include <atomic>','#include <atomic>\nstd::atomic<uint16_t> chakshuConnectionHandle{BLE_HS_CONN_HANDLE_NONE};\nstd::atomic<bool> chakshuAudioSubscribed{false};','Chakshu connection ownership');
  replace('#include <atomic>','#include <atomic>\n'+readTemplate('xiao-sense','ble-health.cpp'),'Retained Chakshu link diagnostics');
  // Diagnostics are defined before the recovery implementation in the shared
  // sketch. Declare this flag before the extended encoder reads it.
  replace('#include <atomic>','#include <atomic>\nextern std::atomic<bool> recoveryWaiting;','Recovery state for early diagnostic encoder');
  out=replaceFunctionBlock(out,'class ServerCallbacks :','class ControlCallbacks :',readTemplate('xiao-sense','ble-link.cpp')+'\n'+readTemplate('xiao-sense','ble-server.cpp')+'\n','Chakshu server callbacks');
  replace('    reconcileConnection();','    reconcileConnection();\n    serviceChakshuLink();','Deferred bounded supervision request');
  out=replaceFunctionBlock(out,'class AudioCallbacks :','class DiagnosticsCallbacks :',readTemplate('xiao-sense','ble-audio.cpp')+'\n','Chakshu audio notifications');
  out=replaceFunctionBlock(out,'class ControlCallbacks :','class AudioCallbacks :',readTemplate('xiao-sense','ble-control.cpp')+'\n','Chakshu command/status separation');
  replace('    queueEvent(EventType::COMMAND, command, version, streamGeneration.load());',
    '    if (command==CMD_GET_STATUS && version==PROTOCOL_VERSION) ChakshuLink::statusSeen=true;\n    queueEvent(EventType::COMMAND, command, version, streamGeneration.load());','Observe core handshake without extra GATT traffic');
  replace('constexpr uint8_t DIAGNOSTICS_VERSION = 2;','constexpr uint8_t DIAGNOSTICS_VERSION = 4;','Chakshu link diagnostics version');
  replace('  uint8_t value[48] = {};','  uint8_t value[84] = {};','Chakshu link diagnostics size');
  replace('  diagnosticsCharacteristic->setValue(value,sizeof(value));',
    '  ChakshuLink::append(value,deviceConnected.load(),streamingEnabled.load()&&!recoveryWaiting.load(),chakshuAudioSubscribed.load(),millis());\n  diagnosticsCharacteristic->setValue(value,sizeof(value));','Append boot and last-link evidence');
  replace('  controlCharacteristic=service->createCharacteristic(CONTROL_CHAR_UUID,\n    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |\n    BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);\n  controlCharacteristic->setCallbacks(new ControlCallbacks());',
    '  controlCharacteristic=new ChakshuControlCharacteristic();\n  service->addCharacteristic(controlCharacteristic);','Register isolated control writes');
  replace('  if (notify && deviceConnected.load()) controlCharacteristic->notify();',
    '  if (notify && deviceConnected.load()) controlCharacteristic->notify(value,sizeof(value),chakshuConnectionHandle.load());','Notify owned status bytes');
  // NimBLE-Arduino invokes write callbacks synchronously with the current value.
  // Take one owned copy so an asynchronous status update cannot replace a command.
  replace('    OtaMessage message{};\n    const size_t size=characteristic->getLength();','    OtaMessage message{};\n    const auto written=characteristic->getValue();\n    const size_t size=written.size();','OTA command snapshot');
  replace('!characteristic->getData()','!written.data()','OTA command pointer');
  replace('memcpy(message.data,characteristic->getData(),size);','memcpy(message.data,written.data(),size);','OTA command bytes');
  replace('    const uint8_t* data=characteristic->getData();const size_t size=characteristic->getLength();','    const auto written=characteristic->getValue();\n    const uint8_t* data=written.data();const size_t size=written.size();','Recovery command snapshot');
  // No hand-managed CCCDs or Bluedroid-specific branches on this target.
  out=out.replace(/#if defined\(CONFIG_BLUEDROID_ENABLED\)\n[\s\S]*?#endif\n/g,'');
  out=out.replace(/\b(BLECharacteristicCallbacks|BLEServerCallbacks|BLECharacteristic|BLEService|BLEServer|BLEAdvertising|BLEDevice|BLEUUID)\b/g,'Nim$1');
  out=out.replace(/NimBLECharacteristic::PROPERTY_(READ|WRITE_NR|WRITE|NOTIFY)/g,'NIMBLE_PROPERTY::$1');
  out=out.replace(/void (onRead|onWrite)\(NimBLECharacteristic\* (\w+)\)/g,'void $1(NimBLECharacteristic* $2, NimBLEConnInfo&)');
  out=out.replace(/const String value=(\w+)->getValue\(\);/g,'const auto value=$1->getValue();');
  out=out.replace(/bleServer->getConnId\(\)/g,'chakshuConnectionHandle.load()');
  replace('bleServer->createService(NimBLEUUID(SERVICE_UUID),64)','bleServer->createService(SERVICE_UUID)','Native service handles');
  replace('#if defined(CONFIG_NIMBLE_ENABLED)\n  bleServer->advertiseOnDisconnect(true);\n#endif',
    '  bleServer->advertiseOnDisconnect(true);','Native reconnect advertising');
  replace("  // Audio/control + device ID + OTA/status/build identity + diagnostics exceed\n  // Bluedroid's default service reservation. NimBLE accepts this overload as well.",
    '  // NimBLE-Arduino sizes the service table from its registered characteristics.','Native service comment');
  replace('  advertising->setScanResponse(true);\n  advertising->setMinPreferred(BLE_MIN_INTERVAL);\n  advertising->setMaxPreferred(BLE_MAX_INTERVAL);',
    '  advertising->enableScanResponse(true);\n  advertising->setName(DEVICE_NAME);\n  advertising->setPreferredParams(BLE_MIN_INTERVAL,BLE_MAX_INTERVAL);\n  advertising->setMinInterval(32);\n  advertising->setMaxInterval(32);','Native advertising');
  replace('  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }',
    '  if (!chakshuAudioSubscribed.load()) { stopStreaming(ErrorCode::AUDIO_NOT_SUBSCRIBED);return; }\n  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }','Require actual audio subscription');
  replace('    audioCharacteristic->setValue(packet, AUDIO_HEADER_BYTES+length);','    // Send this immutable packet to the current subscribed connection.','Owned audio payload');
  replace('      const uint32_t rejectedBefore=notifyRejected.load();\n      // In the pinned Arduino BLE library, onStatus runs before notify returns.\n      // SUCCESS_NOTIFY means queued locally, not persisted by the phone.\n      audioCharacteristic->notify();\n      if(notifyRejected.load()==rejectedBefore) { accepted=true;break; }',
    '      if(sendChakshuAudio(packet,AUDIO_HEADER_BYTES+length)) { accepted=true;break; }','Native audio acceptance');
  // Required capture state must exist before the first client can send START.
  replace('  initializeBLE();\n  initializeRecovery();',
    '  initializeRecovery();','Prepare recovery before advertising');
  replace('    fatalSetup("[FATAL] task allocation failed");\n  }\n}',
    '    fatalSetup("[FATAL] task allocation failed");\n  }\n  initializeBLE();\n}','Advertise after capture tasks are ready');
  if(/getData\(|getConnId\(|#include <BLE|\bBLECharacteristic\b/.test(out))throw Error('Unadapted Chakshu Bluetooth API');
  return out;
}
module.exports={materializeBle};
