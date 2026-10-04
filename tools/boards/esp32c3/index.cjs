'use strict';
const {PRIMARY_TARGET}=require('../../targets.cjs');
const {replaceOnce,replaceFunctionBlock}=require('../../target-source.cjs');

function materializeC3Ble(source){
  let out=source;
  const replace=(before,after,label)=>out=replaceOnce(out,before,after,label);
  replace('#include <BLEDevice.h>','#include <NimBLEDevice.h>','C3 NimBLE library');
  replace('#include <BLEServer.h>','','C3 NimBLE server include');
  replace('#include <BLE2902.h>','','C3 automatic CCCDs');
  replace('BLE2902* audioCccd = nullptr;','','C3 automatic audio CCCD');
  replace('#include <atomic>','#include <atomic>\nstd::atomic<uint16_t> c3ConnectionHandle{BLE_HS_CONN_HANDLE_NONE};\nstd::atomic<bool> c3AudioSubscribed{false},c3MediaSubscribed{false};','C3 NimBLE ownership');
  replace('deviceIdentity->setValue(synapDeviceId);',
    'deviceIdentity->setValue(reinterpret_cast<const uint8_t*>(synapDeviceId),strlen(synapDeviceId));','C3 device ID text bytes');
  replace('identity->setValue(SYNAP_FIRMWARE_ID);',
    'identity->setValue(reinterpret_cast<const uint8_t*>(SYNAP_FIRMWARE_ID),sizeof(SYNAP_FIRMWARE_ID)-1);','C3 firmware identity text bytes');

  const server=[
    'class ServerCallbacks : public NimBLEServerCallbacks {',
    '  void onConnect(NimBLEServer* server,NimBLEConnInfo& peer) override {',
    '    if(deviceConnected.load()) { server->disconnect(peer.getConnHandle());return; }',
    '    c3ConnectionHandle=peer.getConnHandle();c3AudioSubscribed=false;c3MediaSubscribed=false;',
    '    ++connectionGeneration;',
    '    if(!recoveryWaiting.load())streamingEnabled=false;',
    '    deviceConnected=true;connectedLedAt=millis();connectionEventPending=true;',
    '    server->updateConnParams(peer.getConnHandle(),BLE_MIN_INTERVAL,BLE_MAX_INTERVAL,BLE_SLAVE_LATENCY,BLE_SUPERVISION_TIMEOUT);',
    '  }',
    '  void onDisconnect(NimBLEServer*,NimBLEConnInfo& peer,int reason) override {',
    '    if(peer.getConnHandle()!=c3ConnectionHandle.load())return;',
    '    deviceConnected=false;c3AudioSubscribed=false;c3MediaSubscribed=false;c3ConnectionHandle=BLE_HS_CONN_HANDLE_NONE;',
    '    ++linkDisconnects;lastDisconnectAt=millis();lastDisconnectReason=uint16_t(reason);',
    '    if(recoveryEnabled.load()&&streamingEnabled.load()) { recoveryWaiting=true;if(!recoveryWaitingAt.load())recoveryWaitingAt=millis(); }',
    '    else streamingEnabled=false;',
    '    ++connectionGeneration;connectionEventPending=true;',
    '  }',
    '};'
  ].join('\n')+'\n';
  out=replaceFunctionBlock(out,'class ServerCallbacks :','class ControlCallbacks :',server,'C3 NimBLE server callbacks');

  const control=[
    'class C3ControlCharacteristic : public NimBLECharacteristic {',
    ' public:',
    '  C3ControlCharacteristic() : NimBLECharacteristic(CONTROL_CHAR_UUID,',
    '    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY,16) {}',
    ' private:',
    '  void writeEvent(const uint8_t* data,uint16_t length,NimBLEConnInfo& peer) override {',
    '    if(!deviceConnected.load()||peer.getConnHandle()!=c3ConnectionHandle.load())return;',
    '    const uint8_t command=length==2&&data?data[0]:0xFF;',
    '    const uint8_t version=length==2&&data?data[1]:0;',
    '    queueEvent(EventType::COMMAND,command,version,streamGeneration.load());',
    '  }',
    '};'
  ].join('\n')+'\n';
  out=replaceFunctionBlock(out,'class ControlCallbacks :','class AudioCallbacks :',control,'C3 owned control writes');

  const audio=[
    'class AudioCallbacks : public NimBLECharacteristicCallbacks {',
    '  void onSubscribe(NimBLECharacteristic*,NimBLEConnInfo& peer,uint16_t flags) override {',
    '    if(peer.getConnHandle()==c3ConnectionHandle.load())c3AudioSubscribed=(flags&1)!=0;',
    '  }',
    '};',
    'static int submitC3Notification(NimBLECharacteristic* characteristic,const uint8_t* bytes,size_t length) {',
    '  const uint16_t connection=c3ConnectionHandle.load();',
    '  if(!characteristic||!deviceConnected.load()||connection==BLE_HS_CONN_HANDLE_NONE)return -1;',
    '  constexpr int controlReserve=4;',
    '  if(os_msys_num_free()<=controlReserve)return BLE_HS_ENOMEM;',
    '  os_mbuf* packet=ble_hs_mbuf_from_flat(bytes,length);',
    '  if(!packet)return BLE_HS_ENOMEM;',
    '  if(os_msys_num_free()<controlReserve){os_mbuf_free_chain(packet);return BLE_HS_ENOMEM;}',
    '  return ble_gattc_notify_custom(connection,characteristic->getHandle(),packet);',
    '}',
    'static bool sendC3Audio(const uint8_t* bytes,size_t length) {',
    '  if(!c3AudioSubscribed.load())return false;',
    '  const int result=submitC3Notification(audioCharacteristic,bytes,length);',
    '  if(result!=0){lastNotifyStatus=4;lastNotifyError=uint32_t(result);++notifyRejected;return false;}',
    '  return true;',
    '}'
  ].join('\n')+'\n';
  out=replaceFunctionBlock(out,'class AudioCallbacks :','class DiagnosticsCallbacks :',audio,'C3 NimBLE audio notifications');

  const stream=[
    'class StreamCallbacks : public NimBLECharacteristicCallbacks {',
    '  void onSubscribe(NimBLECharacteristic*,NimBLEConnInfo& peer,uint16_t flags) override {',
    '    if(peer.getConnHandle()==c3ConnectionHandle.load())c3MediaSubscribed=(flags&1)!=0;',
    '  }',
    '};',
    'static bool sendC3Media(const uint8_t* bytes,size_t length) {',
    '  if(!c3MediaSubscribed.load())return false;',
    '  const int result=submitC3Notification(streamCharacteristic,bytes,length);',
    '  if(result!=0){++streamNotifyRejected;return false;}',
    '  return true;',
    '}'
  ].join('\n')+'\n';
  out=replaceFunctionBlock(out,'class StreamCallbacks :','static bool sendMediaPacket',stream,'C3 NimBLE SD notifications');

  replace('    OtaMessage message{};\n    const size_t size=characteristic->getLength();',
    '    OtaMessage message{};\n    const auto written=characteristic->getValue();\n    const size_t size=written.size();','C3 OTA write snapshot');
  replace('!characteristic->getData()','!written.data()','C3 OTA command pointer');
  replace('memcpy(message.data,characteristic->getData(),size);','memcpy(message.data,written.data(),size);','C3 OTA command bytes');
  replace('    const uint8_t* data=characteristic->getData();const size_t size=characteristic->getLength();',
    '    const auto written=characteristic->getValue();\n    const uint8_t* data=reinterpret_cast<const uint8_t*>(written.data());const size_t size=written.size();','C3 recovery write snapshot');
  replace('    const size_t length=characteristic->getLength();\n    const uint8_t* p=characteristic->getData();',
    '    const auto written=characteristic->getValue();\n    const size_t length=written.size();\n    const uint8_t* p=reinterpret_cast<const uint8_t*>(written.data());','C3 SD command snapshot');

  replace('  controlCharacteristic=service->createCharacteristic(CONTROL_CHAR_UUID,\n    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |\n    BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);\n  controlCharacteristic->setCallbacks(new ControlCallbacks());',
    '  controlCharacteristic=new C3ControlCharacteristic();\n  service->addCharacteristic(controlCharacteristic);','C3 isolated control characteristic');

  const audioSend='    audioCharacteristic->setValue(packet, AUDIO_HEADER_BYTES+length);\n    bool accepted=false;\n    for(uint8_t attempt=0;attempt<4;++attempt) {\n      if(attempt)vTaskDelay(pdMS_TO_TICKS(15u*attempt));\n      if(!streamingEnabled.load() || !deviceConnected.load() ||\n          generation!=streamGeneration.load() || connection!=connectionGeneration.load() || replay!=audioReplayGeneration.load())return false;\n      const uint32_t rejectedBefore=notifyRejected.load();\n      // In the pinned Arduino BLE library, onStatus runs before notify returns.\n      // SUCCESS_NOTIFY means queued locally, not persisted by the phone.\n      audioCharacteristic->notify();\n      if(notifyRejected.load()==rejectedBefore) { accepted=true;break; }\n    }';
  const nimAudio='    bool accepted=false;\n    for(uint8_t attempt=0;attempt<4;++attempt) {\n      if(attempt)vTaskDelay(pdMS_TO_TICKS(15u*attempt));\n      if(!streamingEnabled.load() || !deviceConnected.load() ||\n          generation!=streamGeneration.load() || connection!=connectionGeneration.load() || replay!=audioReplayGeneration.load())return false;\n      if(sendC3Audio(packet,AUDIO_HEADER_BYTES+length)){accepted=true;break;}\n    }';
  replace(audioSend,nimAudio,'C3 native audio submission');

  const mediaSend='  streamCharacteristic->setValue(packet,16+size);\n  for (uint8_t attempt=0;attempt<4;++attempt) {\n    if (request.connection!=connectionGeneration.load() || !deviceConnected.load() ||\n        request.windowEpoch!=cancelWindow.load()) return false;\n    if (attempt) vTaskDelay(pdMS_TO_TICKS(8u*attempt));\n    const uint32_t rejectedBefore=streamNotifyRejected.load();\n    streamCharacteristic->notify();\n    if (streamNotifyRejected.load()==rejectedBefore) return true;\n  }';
  const nimMedia='  for (uint8_t attempt=0;attempt<4;++attempt) {\n    if (request.connection!=connectionGeneration.load() || !deviceConnected.load() ||\n        request.windowEpoch!=cancelWindow.load()) return false;\n    if (attempt) vTaskDelay(pdMS_TO_TICKS(8u*attempt));\n    if (sendC3Media(packet,16+size)) return true;\n  }';
  replace(mediaSend,nimMedia,'C3 native SD submission');

  out=out.replace(/#if defined\(CONFIG_BLUEDROID_ENABLED\)\n[\s\S]*?#endif\n/g,'');
  out=out.replace(/\b(BLECharacteristicCallbacks|BLEServerCallbacks|BLECharacteristic|BLEService|BLEServer|BLEAdvertising|BLEDevice|BLEUUID)\b/g,'Nim$1');
  out=out.replace(/NimBLECharacteristic::PROPERTY_(READ|WRITE_NR|WRITE|NOTIFY)/g,'NIMBLE_PROPERTY::$1');
  out=out.replace(/void (onRead|onWrite)\(NimBLECharacteristic\* (\w+)\) override/g,'void $1(NimBLECharacteristic* $2, NimBLEConnInfo&) override');
  out=out.replace(/bleServer->getConnId\(\)/g,'c3ConnectionHandle.load()');
  replace('bleServer->createService(NimBLEUUID(SERVICE_UUID),64)','bleServer->createService(SERVICE_UUID)','C3 native service handles');
  replace('#if defined(CONFIG_NIMBLE_ENABLED)\n  bleServer->advertiseOnDisconnect(true);\n#endif',
    '  bleServer->advertiseOnDisconnect(true);','C3 reconnect advertising');
  replace('  advertising->setScanResponse(true);\n  advertising->setMinPreferred(BLE_MIN_INTERVAL);\n  advertising->setMaxPreferred(BLE_MAX_INTERVAL);',
    '  advertising->enableScanResponse(true);\n  advertising->setName(DEVICE_NAME);\n  advertising->setPreferredParams(BLE_MIN_INTERVAL,BLE_MAX_INTERVAL);','C3 native advertising');

  const forbiddenC3Api=[
    ['getData(',/getData\(/],['getLength(',/getLength\(/],['getConnId(',/getConnId\(/],
    ['#include <BLE',/#include <BLE/],['BLECharacteristic',/\bBLECharacteristic\b/],
    ['BLE2902',/BLE2902/],['SUCCESS_NOTIFY',/SUCCESS_NOTIFY/],['onStatus(',/onStatus\(/],
  ];
  const leftover=forbiddenC3Api.find(([,pattern])=>pattern.test(out));
  if(leftover) throw Error('Unadapted C3 Bluetooth API: '+leftover[0]);
  return out;
}

function materializeC3(source,target){
  let out=replaceOnce(source,'p[21]!=9 || p[22]!=0',`p[21]!=${target.chip} || p[22]!=0`,'ESP image chip ID');
  out=replaceOnce(out,'analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_6db);',
    `analogSetPinAttenuation(BATTERY_ADC_PIN, ${target.hardware.batteryAttenuation});`,'ADC input range');

  const taskBefore=`  if (xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle, 0) != pdPASS ||
      xTaskCreatePinnedToCore(transmitterTask, "transmit", 8192, nullptr, 2, nullptr, 1) != pdPASS) {`;
  const taskAfter=`  // ESP32-C3 has one core; retain task priorities and stack sizes without pinning.
  if (xTaskCreate(controlTask, "control", 8192, nullptr, 3, nullptr) != pdPASS ||
      xTaskCreate(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle) != pdPASS ||
      xTaskCreate(transmitterTask, "transmit", 8192, nullptr, 2, nullptr) != pdPASS) {`;
  out=replaceOnce(out,taskBefore,taskAfter,'single-core task creation');
  // Offline SD audio owns the C3 CPU while the shared S3/Chakshu source
  // retains its independent power-profile materialization anchor.
  out=replaceOnce(out,'applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu());',
    'applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu() || odysseyRecording.load());',
    'C3 SD capture CPU profile');

  if(out.includes(PRIMARY_TARGET))throw Error('C3 source still contains the S3 target identity');
  if(out.includes('SYNAP-ESP32S3-OTA-ID-V3'))throw Error('C3 source still contains the S3 product marker');
  if(out.includes('esp_sleep_enable_ext1_wakeup'))throw Error('C3 source still contains unsupported EXT1 wake');
  if(!out.includes(`"SYNAP-FW:${target.id}:" SYNAP_VERSION`))throw Error('C3 firmware identity was not materialized');
  if(!out.includes(target.productMarker))throw Error('C3 OTA marker was not materialized');
  if(!out.includes('p[21]!=5 || p[22]!=0'))throw Error('C3 chip image check was not materialized');
  if(!out.includes('esp_deep_sleep_enable_gpio_wakeup'))throw Error('C3 GPIO deep-sleep wake is unavailable');
  if(!out.includes('long press -> DEEP SLEEP'))throw Error('C3 long-press power gesture was not materialized');
  if(!out.includes('double tap -> START'))throw Error('C3 double-tap recording gesture was not materialized');
  out=materializeC3Ble(out);
  if(!out.includes('#include <NimBLEDevice.h>') || out.includes('#include <BLEDevice.h>'))
    throw Error('C3 NimBLE transport was not materialized');
  return out;
}

module.exports={materializeC3};
