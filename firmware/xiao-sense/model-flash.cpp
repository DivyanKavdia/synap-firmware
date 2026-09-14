// Release preparation embeds lossless DEFLATE weights in the application image.
// Both OTA slots retain a complete code/model pair; no partition is rewritten.
#define SYNAP_VOICE_FLASH 0
#if SYNAP_VOICE_FLASH
#include <miniz.h>
#endif
namespace ChakshuFlashModel {
// SYNAP_EMBEDDED_VOICE_MODEL
constexpr bool present(){return SYNAP_VOICE_FLASH==1;}
enum LoadResult { LOADED,MISSING,NO_MEMORY,INVALID };
#if SYNAP_VOICE_FLASH
LoadResult inflate(const uint8_t* input,size_t length,uint8_t* output,size_t capacity) {
  if(!input||!length||!output||capacity!=ChakshuModel::MODEL_BYTES)return INVALID;
  // The ROM decoder's state is too large for the Arduino setup task's stack.
  auto* decoder=static_cast<tinfl_decompressor*>(calloc(1,sizeof(tinfl_decompressor)));
  if(!decoder)return NO_MEMORY;
  tinfl_init(decoder);
  size_t inputAt=0,outputAt=0;
  LoadResult result=INVALID;
  for(;;){
    size_t inputSize=length-inputAt;
    size_t outputSize=std::min(size_t(4096),capacity-outputAt);
    const auto state=tinfl_decompress(decoder,input+inputAt,&inputSize,
      output,output+outputAt,&outputSize,TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    inputAt+=inputSize;outputAt+=outputSize;
    if(state==TINFL_STATUS_DONE){
      if(inputAt==length&&outputAt==capacity)result=LOADED;
      break;
    }
    if(state!=TINFL_STATUS_HAS_MORE_OUTPUT||(!inputSize&&!outputSize)||outputAt>=capacity)break;
    vTaskDelay(1);
  }
  free(decoder);return result;
}
#endif
LoadResult load(uint8_t* output,size_t capacity) {
#if SYNAP_VOICE_FLASH
  return inflate(DATA,sizeof(DATA),output,capacity);
#else
  (void)output;(void)capacity;return MISSING;
#endif
}
}
