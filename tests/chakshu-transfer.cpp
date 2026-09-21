#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace ChakshuMedia {enum {NO_SD=3,IO_ERROR=7,FILE_UNAVAILABLE=11};}
constexpr int CARD_NONE=0;
unsigned mounted=1,handles=0;
namespace ChakshuStorage {
bool ready=true;bool protectedCapture=false;
void protect(const char*){protectedCapture=true;}
void clearProtection(){protectedCapture=false;}
bool begin(bool){return ready;}
bool recoverIO(){if(!ready)return false;++mounted;return true;}
}
constexpr int FILE_READ=0;
bool exists=true,cardPresent=true,shortRead=false,canSeek=true,directory=false;
std::vector<uint8_t> disk(1000);
struct File {
 bool opened=false;unsigned epoch=0;size_t at=0;
 explicit File(bool open):opened(open),epoch(mounted){if(opened)++handles;}
 File(const File&)=delete;
 ~File(){close();}
 explicit operator bool()const{return opened;}
 bool isDirectory(){assert(opened&&epoch==mounted);return directory;}
 size_t size(){assert(opened&&epoch==mounted);return disk.size();}
 bool seek(size_t offset){assert(opened&&epoch==mounted);at=offset;return canSeek;}
 int read(uint8_t* bytes,size_t size){assert(opened&&epoch==mounted);if(shortRead)return 0;memcpy(bytes,disk.data()+at,size);return size;}
 void close(){if(opened){assert(epoch==mounted);--handles;opened=false;}}
};
struct {
 int cardType(){return cardPresent?1:CARD_NONE;}
 bool exists(const char*){return ::exists;}
 File open(const char*,int){return File(::exists);}
} SD;
char selectedPath[64]{};uint8_t* buffer=nullptr;size_t bufferSize=0;
// INSERT FILE HELPERS
int main(){
 for(size_t i=0;i<disk.size();++i)disk[i]=uint8_t(i);
 uint32_t total=0;uint8_t bytes[480];size_t size=0;
 assert(selectFile("/synap/12345678-12345678.wav",total)==0&&total==1000&&handles==0&&ChakshuStorage::protectedCapture);
 assert(readSelection(0,total,bytes,size)==0&&size==480&&bytes[479]==uint8_t(479)&&handles==0);
 // Hardware refresh/remount while the PWA waits for the next chunk.
 ++mounted;
 assert(readSelection(480,total,bytes,size)==0&&size==480&&bytes[0]==uint8_t(480)&&handles==0);
 assert(readSelection(960,total,bytes,size)==0&&size==40&&bytes[39]==uint8_t(999)&&handles==0);
 assert(readSelection(1000,total,bytes,size)==11&&handles==0);
 canSeek=false;assert(readSelection(0,total,bytes,size)==7&&handles==0);canSeek=true;
 shortRead=true;assert(readSelection(0,total,bytes,size)==7&&handles==0);shortRead=false;
 exists=false;assert(readSelection(0,total,bytes,size)==11&&handles==0);exists=true;
 cardPresent=false;assert(readSelection(0,total,bytes,size)==3&&handles==0&&!ChakshuStorage::ready);cardPresent=true;
 ChakshuStorage::ready=false;assert(readSelection(0,total,bytes,size)==3&&handles==0);
 clearSelection();buffer=static_cast<uint8_t*>(malloc(3));bufferSize=3;memcpy(buffer,"jpg",3);
 assert(readSelection(1,total,bytes,size)==0&&total==3&&size==2&&bytes[0]=='p');clearSelection();
 assert(!buffer&&!bufferSize&&!selectedPath[0]&&handles==0&&!ChakshuStorage::protectedCapture);
 puts("PASS SD remount, bounds, missing card, failure cleanup and RAM photo reads");
}
