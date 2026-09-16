'use strict';
// Narrow fixes for Arduino ESP32 3.3.5's built-in NimBLE compatibility layer.
// Keep the stack/version pinned; do not silently apply this to another release.
const fs = require('node:fs'), path = require('node:path'), crypto = require('node:crypto');
const hashes = {
  'BLECharacteristic.cpp': '570f904653fd9862ec9342904581702780ce41c9274059f607e33bacf890e644',
  'BLECharacteristic.h': '1957fa5b9607c1cb092f307989712868f3972efe2b8cdbe73d7f259d6fe5634e',
};
const patchedHashes = {
  "BLECharacteristic.cpp": "4e19b177c7295a865a14c8c455508f98ad9239cc2a6ef3e961a602d1490594b1",
  "BLECharacteristic.h": "5d93e5cd3d340ea7127ff851a8b195a241ad818799083189e4f7e8e6ec4009d5"
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
