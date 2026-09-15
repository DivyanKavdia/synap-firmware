// The control UUID carries commands on write and status on read/notify. NimBLE's
// default writeEvent stores a command before onWrite, overwriting that status.
// Consume the owned write bytes directly and leave the readable status intact.
class ChakshuControlCharacteristic : public NimBLECharacteristic {
 public:
  ChakshuControlCharacteristic() : NimBLECharacteristic(CONTROL_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY, 16) {}

 private:
  void writeEvent(const uint8_t* data, uint16_t length, NimBLEConnInfo& peer) override {
    if (!deviceConnected.load() || peer.getConnHandle() != chakshuConnectionHandle.load()) return;
    const uint8_t command = length == 2 && data ? data[0] : 0xFF;
    const uint8_t version = length == 2 && data ? data[1] : 0;
    if(command==CMD_STOP && version==PROTOCOL_VERSION)++ChakshuTransfer::cancelWindow;
    queueEvent(EventType::COMMAND, command, version, streamGeneration.load());
  }
};
