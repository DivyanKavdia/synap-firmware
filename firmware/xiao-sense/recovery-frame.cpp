  // A producer may evict the ring head while BLE is retrying its fragments.
  // Keep this frame's PCM until its last fragment is accepted. Otherwise every
  // eviction restarts chunk zero and a slow link can stop completing frames.
  static SynapRecovery::StoredFrame frame;
  static bool held=false;
  static uint32_t heldConnection=0,heldReplay=0;
  const uint32_t replay=chakshuAudioReplayGeneration.load();
  uint16_t pending=0;
  {
    RecoveryGuard guard;
    if(!held || heldConnection!=connection || heldReplay!=replay || frame.generation!=streamGeneration.load()) {
      held=false;
      if(!recoveryRing.peek(frame))return false;
      held=true;heldConnection=connection;heldReplay=replay;
    }
    pending=recoveryRing.count-recoveryRing.cursor;
  }
