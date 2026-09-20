// XIAO's onboard user LED is active-low; never drive the camera's GPIO48.
// Called by the normal control tick, independently of blocking SD writes.
void updateStatusLed(bool force) {
  (void)force;
  // Keep indicating an existing SD session while it closes after BLE reconnect.
  const bool on=ChakshuTransfer::offline.load() && millis()%1000u<250u;
  digitalWrite(21,on?LOW:HIGH);
}

