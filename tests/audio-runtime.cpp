#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <algorithm>
constexpr uint16_t SAMPLES_PER_FRAME=800, ADPCM_HEADER_BYTES=4, ADPCM_BYTES_PER_FRAME=404;
constexpr uint16_t TRANSPORT_BYTES_PER_FRAME=404, MAX_AUDIO_PAYLOAD_BYTES=500;
constexpr uint8_t AUDIO_CODEC_IMA_ADPCM=1, AUDIO_HEADER_BYTES=8, AUDIO_PACKET_MAGIC=0xA5;
constexpr uint8_t AUDIO_PROTOCOL_VERSION=3, MIN_CHUNKS_PER_FRAME=1, MAX_CHUNKS_PER_FRAME=20;
struct AudioFrame { uint32_t generation; uint16_t sequence; int16_t samples[800]; };
std::atomic<bool> streamingEnabled{true},deviceConnected{true};
std::atomic<uint32_t> streamGeneration{1};
std::atomic<uint16_t> audioPayloadBytes{404},attValueCapacity{514};
std::atomic<uint8_t> chunksPerFrame{1};
uint32_t clockNow=0,spinMicros=0;
bool cancelOnNotify=false;
uint32_t micros(){return clockNow;}
void vTaskDelay(unsigned ticks){clockNow+=ticks*1000;}
void delayMicroseconds(unsigned us){clockNow+=us;spinMicros+=us;}
struct Packet { uint32_t time;std::vector<uint8_t> bytes; };
struct Characteristic {
  std::vector<uint8_t> pending;
  std::vector<Packet> packets;
  void setValue(const uint8_t* p,unsigned n){pending.assign(p,p+n);}
  void notify(){packets.push_back({clockNow,pending});if(cancelOnNotify)++streamGeneration;}
} characteristic;
auto* audioCharacteristic=&characteristic;
// INSERT CODEC AND TRANSPORT
int main(){
  uint32_t random=0x579AB13Cu;
  uint64_t hash=14695981039346656037ull;
  AudioFrame frame{};frame.generation=1;frame.sequence=65535;
  for(unsigned test=0;test<256;++test){
    for(unsigned i=0;i<800;++i){
      random^=random<<13;random^=random>>17;random^=random<<5;
      const int32_t sample=int32_t(random&65535)-32768;
      frame.samples[i]=test==0?0:test==1?32767:test==2?-32768:test==3?(i&1?-32768:32767):static_cast<int16_t>(sample);
    }
    uint8_t a[406],b[406];memset(a,0xAA,sizeof(a));memset(b,0x55,sizeof(b));
    assert(encodeImaAdpcm(frame.samples,a+1)==404);
    assert(encodeImaAdpcm(frame.samples,b+1)==404);
    assert(a[0]==0xAA && a[405]==0xAA && b[0]==0x55 && b[405]==0x55);
    assert(memcmp(a+1,b+1,404)==0 && (a[404]&0xF0)==0);
    for(unsigned i=1;i<=404;++i){hash^=a[i];hash*=1099511628211ull;}
  }
  for(uint16_t mtu: {32,64,100,185,247,517}){
    attValueCapacity=mtu-3;
    const unsigned bounded=std::min<unsigned>(attValueCapacity-8,500);
    chunksPerFrame=(404+bounded-1)/bounded;
    audioPayloadBytes=(404+chunksPerFrame-1)/chunksPerFrame;
    for(uint32_t initial: {0u,0xFFFFF000u}){
      clockNow=initial;spinMicros=0;characteristic.packets.clear();
      assert(sendAudioFrame(frame,frame.sequence));
      const uint32_t duration=clockNow-initial;
      assert(duration>=45000 && duration<46000);
      assert(characteristic.packets.size()==chunksPerFrame);
      if(chunksPerFrame==1)assert(spinMicros==0);
      uint8_t encoded[404];encodeImaAdpcm(frame.samples,encoded);
      unsigned received=0;
      for(unsigned i=0;i<characteristic.packets.size();++i){
        const auto& packet=characteristic.packets[i];const auto& bytes=packet.bytes;
        assert(bytes.size()<=attValueCapacity && bytes[0]==0xA5 && bytes[1]==3);
        assert(bytes[2]==255 && bytes[3]==255 && bytes[4]==i && bytes[5]==chunksPerFrame);
        const unsigned size=unsigned(bytes[6])+(unsigned(bytes[7])<<8);
        assert(size+8==bytes.size() && received+size<=404);
        assert(memcmp(bytes.data()+8,encoded+received,size)==0);received+=size;
        if(i)assert(uint32_t(packet.time-initial)>=i*45000u/chunksPerFrame);
      }
      assert(received==404);
      // A queued frame must not burst out before the previous frame's pacing window.
      assert(sendAudioFrame(frame,frame.sequence));
      assert(uint32_t(characteristic.packets[chunksPerFrame].time-initial)>=45000);
    }
  }
  chunksPerFrame=20;audioPayloadBytes=21;attValueCapacity=29;
  characteristic.packets.clear();cancelOnNotify=true;
  assert(!sendAudioFrame(frame,frame.sequence));assert(characteristic.packets.size()==1);
  std::cout<<"PASS runtime; codec golden="<<std::hex<<hash<<"\n";
}
