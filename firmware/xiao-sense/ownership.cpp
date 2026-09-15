// One admission gate orders START/OTA transitions against camera, SD and model jobs.
// Persistent workers retain the gate until completion; transitions publish their
// atomic streaming/OTA state before releasing it. No BLE callback waits on it.
namespace ChakshuResources {
std::atomic<bool> media{false},runtimeReady{false};
class Lease {
  bool owned=false;
 public:
  Lease() { bool expected=false;owned=media.compare_exchange_strong(expected,true); }
  ~Lease() { if(owned)media.store(false); }
  explicit operator bool() const { return owned; }
  Lease(const Lease&)=delete;
  Lease& operator=(const Lease&)=delete;
};
}
