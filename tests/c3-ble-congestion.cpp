#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
constexpr uint16_t PCM_MIN_MTU=185,MIN_REQUIRED_MTU=32,MAX_AUDIO_PAYLOAD_BYTES=500;
constexpr uint8_t AUDIO_HEADER_BYTES=8,MAX_CHUNKS_PER_FRAME=20;
constexpr uint16_t AUDIO_BYTES_PER_FRAME=1600,ADPCM_BYTES_PER_FRAME=404;
std::atomic<bool> deviceConnected{true};
std::atomic<uint16_t> peerMtu{23},attValueCapacity{20},audioPayloadBytes{0};
std::atomic<uint8_t> chunksPerFrame{0};
std::atomic<bool> pcmTransport{false};
struct Server {uint16_t mtu=517; uint16_t getPeerMTU(int){return mtu;} int getConnId(){return 0;}} server;
auto* bleServer=&server;
// INSERT TRANSPORT
int main(){
  for(uint16_t mtu:{32u,64u,185u,247u,517u}){
    server.mtu=mtu;
    assert(configureTransportFromPeerMtu());
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
    assert(!pcmTransport.load());
    assert(chunksPerFrame==(mtu==517u?1u:((404u+audioPayloadBytes-1u)/audioPayloadBytes)));
    if(mtu==517u){assert(audioPayloadBytes==404 && attValueCapacity==514 && chunksPerFrame==1);}
#else
    assert(pcmTransport.load()==(mtu>=PCM_MIN_MTU));
    if(mtu==517u)assert(audioPayloadBytes==400 && chunksPerFrame==4);
#endif
    const uint16_t bytes=pcmTransport.load()?1600:404;
    assert((chunksPerFrame-1u)*audioPayloadBytes<bytes);
    assert(chunksPerFrame*audioPayloadBytes>=bytes);
  }
  std::puts("PASS C3 ADPCM congestion-safe high-MTU and S3 PCM preservation");
}
