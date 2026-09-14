// Model upload protocol 1 pins one model; packets never contain filesystem paths.
namespace ChakshuModel {
constexpr size_t MODEL_BYTES=2177224;
constexpr char MODEL_SHA256[]="9bb7348b31891a89eb494f5995970a7fc52b765759e4992d471ab2901bf9c47c";
enum State : uint8_t { AVAILABLE=0,RECEIVING=1,VERIFYING=2,INSTALLED=3,FAILED=4,RESTARTING=5 };
enum Error : uint8_t { OK=0,BUSY=1,NO_SD=2,NO_SPACE=3,IO_ERROR=4,BAD_OFFSET=5,HASH_MISMATCH=6,CANCELLED=7,TIMED_OUT=8,BAD_PACKET=9 };
struct Backend {
  virtual ~Backend()=default;
  virtual Error begin()=0;
  virtual bool write(const uint8_t*,size_t)=0;
  virtual Error finish()=0;
  virtual void abort()=0;
};
class Upload {
  Backend& backend;
  uint32_t owner=0,last=0,lastOffset=0;
  size_t lastLength=0;
  uint8_t previous[480]{};
public:
  State state=AVAILABLE;Error error=OK;
  uint32_t session=0,offset=0;
  explicit Upload(Backend& b):backend(b){}
  bool busy()const{return state==RECEIVING||state==VERIFYING;}
  static uint32_t u32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  void fail(Error e){backend.abort();state=FAILED;error=e;lastLength=0;}
  void tick(uint32_t now){if(busy()&&uint32_t(now-last)>900000u)fail(TIMED_OUT);}
  void packet(const uint8_t* p,size_t n,uint32_t connection,uint32_t now) {
    if(!p||n<5||n>489)return;
    const uint8_t op=p[0];const uint32_t id=u32(p+1);
    if(!id)return;
    if(op==1&&n==5&&!busy()){
      const Error result=backend.begin();
      session=id;offset=0;error=result;lastLength=0;
      if(result!=OK){state=FAILED;return;}
      state=RECEIVING;owner=connection;last=now;return;
    }
    if(id!=session)return;
    if(op==6&&n==5&&busy()){owner=connection;last=now;return;}
    if(!busy()||owner!=connection)return;
    if(op==5&&n==5){fail(CANCELLED);return;}
    if(op==1&&n==5){last=now;return;} // Lost BEGIN acknowledgement.
    if(op==2&&state==RECEIVING&&n>9){
      const uint32_t at=u32(p+5);const size_t size=n-9;
      if(lastLength==size&&at==lastOffset&&!memcmp(previous,p+9,size)){last=now;return;}
      if(at!=offset||size>MODEL_BYTES-offset){fail(BAD_OFFSET);return;}
      if(!backend.write(p+9,size)){fail(IO_ERROR);return;}
      lastOffset=at;lastLength=size;memcpy(previous,p+9,size);offset+=size;last=now;return;
    }
    if(op==3&&n==5&&state==RECEIVING&&offset==MODEL_BYTES){state=VERIFYING;last=now;return;}
    fail(BAD_PACKET);
  }
  void verify(){
    if(state!=VERIFYING)return;
    const Error result=backend.finish();
    if(result!=OK)fail(result);else {state=INSTALLED;error=OK;}
  }
};
}
