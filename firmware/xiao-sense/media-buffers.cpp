// Single producer / single consumer slots. A slow SD write cannot overwrite
// the PCM or JPEG currently being written; audio overflow is an explicit error.
namespace ChakshuBuffers {
template<class T, uint32_t Capacity> class Queue {
  std::atomic<uint32_t> written{0},read{0};
 public:
  T* slots=nullptr;
  T* reserve() {
    const uint32_t w=written.load(std::memory_order_relaxed);
    return slots && w-read.load(std::memory_order_acquire)<Capacity?slots+w%Capacity:nullptr;
  }
  void publish() { written.fetch_add(1,std::memory_order_release); }
  T* peek() {
    const uint32_t r=read.load(std::memory_order_relaxed);
    return r!=written.load(std::memory_order_acquire)?slots+r%Capacity:nullptr;
  }
  void release() { read.fetch_add(1,std::memory_order_release); }
};
}
