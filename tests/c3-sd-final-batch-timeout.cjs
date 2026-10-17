'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');

const recorder=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const assembled=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');

test('C3 final padded write does not fclose a card quarantined by CMD25 timeout',()=>{
  for(const source of [recorder,assembled]) {
    const take=source.split('static void odysseyRecordTake() {')[1]
      .split('static void odysseyRecordTask(void*) {')[0];
    assert(take,'missing active C3 offline recorder');
    const tail=take.indexOf('if (!failed && batchUsed) {');
    const check=take.indexOf('if (odysseySdWriteTimeoutQuarantined() || uncertainSdWrite) {',tail);
    const close=take.indexOf('if (fclose(file)!=0)',tail);
    assert(tail>=0 && check>tail && close>check,
      'recheck write timeout after final padded 2 KiB fwrite, BEFORE fclose');
    const gate=take.slice(check,close);
    assert.match(gate,/odysseySdBootState=2/);
    assert.match(gate,/odysseySdUnsafeToSleep=true/);
    assert.match(gate,/file=nullptr/);
    assert.match(gate,/else \{\s*errno=0/);
    assert.match(take,/if \(file && \(odysseySdWriteTimeoutQuarantined\(\) \|\| uncertainSdWrite\)\)/,
      'initial fast-fail after a mid-take write/checkpoint timeout stays intact');
    assert.match(take,/memset\(batch\+batchUsed,0,sizeof\(batch\)-batchUsed\)/,
      'preserve append-only final multi-sector batch');
  }
});
