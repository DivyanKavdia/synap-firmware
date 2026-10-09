'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const root=path.join(__dirname,'..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');
const cpu=read('firmware/shared/cpu-power.cpp');
const ble=read('firmware/shared/ble-control.cpp');
const boot=read('firmware/shared/boot.cpp');
const fixture=read('tests/c3-ble-connection.cpp').replace('// INSERT CPU',cpu);
for(const [target,flags] of [['c3',['-DCONFIG_IDF_TARGET_ESP32C3=1','-DSYNAP_CHAKSHU=0']],['s3',['-DCONFIG_IDF_TARGET_ESP32C3=0','-DSYNAP_CHAKSHU=0']]]) {
 test(`connection-aware CPU profile and 12-second reconnect window (${target})`,()=>{
  assert.match(nativeTest(fixture,flags),/PASS C3 BLE connection/);
 });
}
test('C3 relies on central BLE connection timing without a GAP renegotiation during native discovery',()=>{
 const server=ble.split('class ServerCallbacks : public BLEServerCallbacks {')[1].split('class ControlCallbacks')[0];
 assert.match(server,/#elif defined\(CONFIG_NIMBLE_ENABLED\)[\s\S]*?onConnect\(BLEServer\* server, ble_gap_conn_desc\* desc\) override/);
 assert.match(server,/#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU[\s\S]*?\(void\)server;[\s\S]*?\(void\)desc;[\s\S]*?#else\s+if\(desc\)server->updateConnParams/);
 assert.match(boot,/bleServer->advertiseOnDisconnect\(true\)/);
 assert.match(ble,/reconcileConnection\(\)/);
 assert.match(ble,/restartAdvertising=true;/);
 assert.match(ble,/if \(deviceConnected\.load\(\) && streamingEnabled\.load\(\)/);
});
