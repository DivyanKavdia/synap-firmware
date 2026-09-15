// Called only by the control task, never inside NimBLE's connect callback.
// The pinned host performs DLE after onConnect. Give that startup work a turn
// before asking for the same 15–30 ms / 0 / 6 s policy as the standard S3.
void serviceChakshuLink() {
  static uint32_t generation=0,lastAttemptAt=0;
  static bool finished=false;
  const uint32_t current=connectionGeneration.load();
  const uint16_t handle=chakshuConnectionHandle.load();
  if (!deviceConnected.load() || handle==BLE_HS_CONN_HANDLE_NONE) return;
  if (generation!=current) {
    generation=current;lastAttemptAt=ChakshuLink::connectedAt.load();finished=false;
  }
  const uint32_t now=millis();
  const uint16_t timeout=ChakshuLink::timeout.load();
  if (finished || !timeout || timeout>=BLE_SUPERVISION_TIMEOUT ||
      uint32_t(now-lastAttemptAt)<1000u) return;
  ble_gap_upd_params params{};
  params.itvl_min=BLE_MIN_INTERVAL;params.itvl_max=BLE_MAX_INTERVAL;
  params.latency=BLE_SLAVE_LATENCY;params.supervision_timeout=BLE_SUPERVISION_TIMEOUT;
  params.min_ce_len=BLE_GAP_INITIAL_CONN_MIN_CE_LEN;
  params.max_ce_len=BLE_GAP_INITIAL_CONN_MAX_CE_LEN;
  if (current!=connectionGeneration.load() || handle!=chakshuConnectionHandle.load() ||
      !deviceConnected.load()) return;
  const int rc=ble_gap_update_params(handle,&params);
  // A completed request belonging to an old link must not update its replacement.
  if (current!=connectionGeneration.load() || !deviceConnected.load()) return;
  lastAttemptAt=now;
  const unsigned attempts=++ChakshuLink::paramRequests;
  ChakshuLink::paramRequestCode=uint16_t(rc);
  // rc==0 means submitted, not accepted. Only GAP's observed parameters prove
  // the new timeout. Never fight a central that rejects or later replaces it.
  finished=(rc!=BLE_HS_EBUSY && rc!=BLE_HS_EALREADY) || attempts>=3;
}
