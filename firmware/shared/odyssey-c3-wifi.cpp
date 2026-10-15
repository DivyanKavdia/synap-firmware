// BLE-only Odyssey C3. The former STA/TLS uploader has been removed.
// Keep this compatibility shim while other shared recorder/power modules
// retain their lightweight OdysseyWifi::busy() interlock.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
namespace OdysseyWifi {
void initialize() {}
bool busy() { return false; }
}
#endif
