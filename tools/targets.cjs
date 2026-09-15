'use strict';
const catalog = require('../devices/catalog.json');
function freeze(value) {
  if (value && typeof value === 'object') {
    Object.values(value).forEach(freeze);
    Object.freeze(value);
  }
  return value;
}
if (catalog.schema !== 1) throw Error('Unsupported device catalog');
const FLAGS = freeze(catalog.flags);
const TARGETS = freeze(Object.fromEntries(catalog.devices.map(device => [device.id, device])));
const PRIMARY_TARGET = catalog.primaryTarget;
function getTarget(id) {
  if (!Object.hasOwn(TARGETS, id)) throw Error(`Unknown firmware target: ${id}`);
  return TARGETS[id];
}
function capabilityMask(target) {
  return target.features.reduce((mask, feature) => {
    if (!Object.hasOwn(FLAGS, feature)) throw Error(`Unknown device feature: ${feature}`);
    return mask | FLAGS[feature];
  }, 0);
}
module.exports = { PRIMARY_TARGET, TARGETS, FLAGS, getTarget, capabilityMask };
