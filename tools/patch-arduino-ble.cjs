'use strict';
// Narrow fixes for Arduino ESP32 3.3.5's built-in NimBLE compatibility layer.
// Keep the stack/version pinned; do not silently apply this to another release.
const fs = require('node:fs'), path = require('node:path'), crypto = require('node:crypto');
const hashes = {
  'BLECharacteristic.cpp': 'a9256c5b09e9dfac35415860101d8099b4a5cdead2bbca10a386a7a87aa15cc8',
  'BLECharacteristic.h': '0124ddf91dd59807cdc3e232be4343390c34999a538c6398dd8c8db53dbcf772',
};
const patchedHashes = {
  "BLECharacteristic.cpp": "3abccdb98cb43eee55075324d6f8ca1bd6f761531d418eb0279a28022acfd707",
  "BLECharacteristic.h": "232fe57488d6e80d8d8bf7663299331a31ccfc3e3925fef000a690dbed4edc07"
};
function replaceOnce(source, before, after) {
  if (source.split(before).length !== 2) throw Error('Pinned BLE patch no longer matches');
  return source.replace(before, after);
}
function patch(name, source) {
  const digest = crypto.createHash('sha256').update(source).digest('hex');
  if (digest === patchedHashes[name]) return source;
  if (digest !== hashes[name])
    throw Error('Unsupported Arduino BLE source: ' + name + '. Expected ESP32 3.3.5.');
  if (name.endsWith('.h')) return replaceOnce(source,
    '  virtual void onWrite(BLECharacteristic *pCharacteristic, ble_gap_conn_desc *desc);',
    `  virtual void onWrite(BLECharacteristic *pCharacteristic, ble_gap_conn_desc *desc);
  // SYNAP_OWNED_GATT_WRITES: consume request bytes before the host releases them.
  // Firmware callbacks only enqueue work; they must not block or send notifications.
  virtual void onWriteValue(BLECharacteristic *characteristic, ble_gap_conn_desc *desc,
                            const uint8_t *bytes, size_t length) {
    characteristic->setValue(bytes, length);
    onWrite(characteristic, desc);
  }`);
  source = replaceOnce(source,
    'if (ctxt->om->om_pkthdr_len > 8)',
    'if (conn_handle != BLE_HS_CONN_HANDLE_NONE && ctxt->om->om_len > 0)');
  const start = source.indexOf('        pCharacteristic->setValue(buf, len);');
  const end = source.indexOf('        return 0;', start);
  if (start < 0 || end < 0) throw Error('Missing pinned NimBLE write handler');
  return source.slice(0, start) + `        // SYNAP_OWNED_GATT_WRITES: the delayed callback could observe a newer
        // command, status or battery value. Pass this request's bytes now.
        pCharacteristic->m_pCallbacks->onWriteValue(pCharacteristic, &desc, buf, len);

` + source.slice(end);
}
function install(directory) {
  // Validate both inputs before changing either file.
  const outputs = Object.keys(hashes).map(name => [name, patch(name, fs.readFileSync(path.join(directory, name), 'utf8'))]);
  for (const [name, source] of outputs) fs.writeFileSync(path.join(directory, name), source);
}
if (require.main === module) {
  if (!process.argv[2]) throw Error('Usage: node tools/patch-arduino-ble.cjs <esp32-3.3.5/libraries/BLE/src>');
  install(process.argv[2]);
  console.log('Applied pinned BLE command ownership and live-read fixes');
}
module.exports = { patch, install };
