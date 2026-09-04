const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing firmware anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}

function patch(source){
  let out=source;
  out=replaceOnce(out,
`#define CONTROL_CHAR_UUID "4fa12347-0000-1000-8000-00805f9b34fb"`,
`#define CONTROL_CHAR_UUID "4fa12347-0000-1000-8000-00805f9b34fb"
#define EVENT_CHAR_UUID "4fa1234e-0000-1000-8000-00805f9b34fb"`,
  'event characteristic UUID');

  out=replaceOnce(out,
`BLECharacteristic* controlCharacteristic = nullptr;
BLECharacteristic* diagnosticsCharacteristic = nullptr;`,
`BLECharacteristic* controlCharacteristic = nullptr;
BLECharacteristic* eventCharacteristic = nullptr;
BLECharacteristic* diagnosticsCharacteristic = nullptr;`,
  'event characteristic pointer');

  out=replaceOnce(out,
`  controlCharacteristic->setCallbacks(new ControlCallbacks());
#if defined(CONFIG_BLUEDROID_ENABLED)`,
`  controlCharacteristic->setCallbacks(new ControlCallbacks());
  // Protocol-v2 migration channel: asynchronous pendant events no longer need to
  // overwrite the command/status value. Legacy control notifications remain during
  // the transition so already-installed PWAs continue to receive battery/markers.
  eventCharacteristic=service->createCharacteristic(EVENT_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)`,
  'create event characteristic');

  out=replaceOnce(out,
`  controlCharacteristic->addDescriptor(new BLE2902());
#endif`,
`  controlCharacteristic->addDescriptor(new BLE2902());
  eventCharacteristic->addDescriptor(new BLE2902());
#endif`,
  'event CCCD');

  out=replaceOnce(out,
`  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  // Bluedroid may defer notification transmission. Give the 8-byte battery payload`,
`  if (eventCharacteristic) {
    eventCharacteristic->setValue(value,sizeof(value));
    eventCharacteristic->notify();
  }
  // Compatibility path for PWA builds that predate EVENT_CHAR_UUID.
  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  // Bluedroid may defer notification transmission. Give the 8-byte battery payload`,
  'battery event dual publish');

  out=replaceOnce(out,
`  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  // Memory markers share the control characteristic with status packets. Mirror the`,
`  if (eventCharacteristic) {
    eventCharacteristic->setValue(value,sizeof(value));
    eventCharacteristic->notify();
  }
  // Compatibility path for PWA builds that predate EVENT_CHAR_UUID.
  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  // Memory markers share the control characteristic with status packets. Mirror the`,
  'memory event dual publish');

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-event-channel.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,patch(source));
  console.log('Added Synap dedicated BLE event channel');
}
module.exports={patch};
