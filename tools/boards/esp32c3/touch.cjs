'use strict';
const {replaceFunctionBlock,readTemplate}=require('../../target-source.cjs');

function apply(source){
  let out=source;
  const c3Wake=readTemplate('esp32c3','wake.cpp')+'\n';
  out=replaceFunctionBlock(out,'bool confirmTouchWakeTripleTap() {','void publishPowerEvent',c3Wake,'C3 wake gesture');

  const c3Touch=readTemplate('esp32c3','touch.cpp');
  out=replaceFunctionBlock(out,'void pollTouchControl() {','void updateStatusCharacteristic',c3Touch,'C3 awake touch gesture');

  return out;
}

module.exports={apply};
