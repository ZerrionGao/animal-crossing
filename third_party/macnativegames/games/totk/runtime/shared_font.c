/* nn::pl 共享字体：ACNH 先验证原始 BFTTF 文件的 SHA-256，再只解码内存副本中的 XOR 载荷，磁盘原件保持不变。
   GetSharedFontAddress 指向文件头之后的载荷（起始偏移 +8），大小为字体载荷长度。 */
#include "runtime.h"
#include "shared_fonts_config.h"
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct{const char *path;uint64_t file_bytes,font_bytes;const char *sha256;} FontRow;
typedef struct{uint32_t type;uint32_t reserved;const void *address;size_t size;} SharedFontData;
_Static_assert(sizeof(SharedFontData)==0x18,"SharedFontData ABI size");
static const FontRow rows[SHARED_FONT_COUNT]={SHARED_FONT_ROWS};
static uint8_t *region;static uint64_t offsets[SHARED_FONT_COUNT];
static pthread_once_t once=PTHREAD_ONCE_INIT;
#ifdef GAME_ACNH
static pthread_once_t shared_font_order_notice_once=PTHREAD_ONCE_INIT;
#endif
static void need(int c,const char *m){if(!c)runtime_fail(m);}
#ifdef GAME_ACNH
static uint32_t read_be32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void write_be32(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
#endif
static void load(void){
 uint64_t total=0;
 for(int i=0;i<SHARED_FONT_COUNT;i++){
#ifdef GAME_ACNH
  need(rows[i].font_bytes<=UINT64_MAX-8&&rows[i].file_bytes==rows[i].font_bytes+8,"shared font configured size mismatch");
  need(rows[i].font_bytes>=4,"shared font payload too small");
  need((rows[i].font_bytes&3)==0,"shared font decoded size alignment");
#endif
  need(rows[i].file_bytes<=UINT64_MAX-total,"shared font total size overflow");
  total+=rows[i].file_bytes;
 }
 region=malloc(total);need(region!=NULL,"shared font memory");
 uint64_t at=0;
 for(int i=0;i<SHARED_FONT_COUNT;i++){
  /* TOTK_SHARED_FONTS=<dir>: the same verified files in another directory (iOS: copied into the app's Documents). */
  char path[4096];const char *dir=getenv("TOTK_SHARED_FONTS"),*base=strrchr(rows[i].path,'/');
  if(dir)need(snprintf(path,sizeof path,"%s/%s",dir,base?base+1:rows[i].path)<(int)sizeof path,"shared font path");
  FILE *f=fopen(dir?path:rows[i].path,"rb");need(f!=NULL,"open shared font");
  need(fread(region+at,1,rows[i].file_bytes,f)==rows[i].file_bytes&&fgetc(f)==EOF,"shared font size");fclose(f);
  uint8_t d[32];char hex[65];CC_SHA256(region+at,(CC_LONG)rows[i].file_bytes,d);for(int k=0;k<32;k++)snprintf(hex+k*2,3,"%02x",d[k]);
  need(!strcmp(hex,rows[i].sha256),"shared font integrity mismatch");
#ifdef GAME_ACNH
  uint32_t key=read_be32(region+at)^0x7f9a0218u;
  need((read_be32(region+at+4)^key)==rows[i].font_bytes,"shared font decoded size mismatch");
  for(uint64_t p=0;p<rows[i].font_bytes;p+=4)write_be32(region+at+8+p,read_be32(region+at+8+p)^key);
  uint32_t sfnt=read_be32(region+at+8);
  need(sfnt==0x00010000u||sfnt==0x4f54544fu,"shared font payload is not a TrueType/OpenType font");
#endif
  offsets[i]=at+8;at+=rows[i].file_bytes;
 }
 fprintf(stderr,"[totk] shared fonts loaded: %d fonts, %llu bytes\n",SHARED_FONT_COUNT,(unsigned long long)total);
}
static int valid(int type){if(type<0||type>=SHARED_FONT_COUNT)runtime_fail("invalid shared font type");pthread_once(&once,load);return type;}
static int load_state(int type){valid(type);return 1;}   /* SharedFontLoadState_Loaded */
static const void *address(int type){int index=valid(type);return region+offsets[index];}
static size_t size(int type){return (size_t)rows[valid(type)].font_bytes;}
#ifdef GAME_ACNH
/* ACNH 的显式请求会检查字体类型，并确保共享字体只初始化一次。 */
static void request_load_acnh(int type){(void)valid(type);}
/* en-US 仍是已观察到的 0..5。zh-Hans 使用公开的简体优先顺序 1,2,0,3,4,5，不是从原服务抓到的优先级。 */
static void log_shared_font_order_notice(void){fprintf(stderr,"[acnh] GetSharedFont uses a compatibility order; this is not a captured original-service priority\n");}
static uint64_t shared_font_language(const char *text){uint64_t value=0;memcpy(&value,text,strlen(text));return value;}
static unsigned char get_shared_font(int *count,SharedFontData *out,int max,uint64_t language){
 if(!count)runtime_fail("shared font count pointer is null");
 static const int en_us_order[SHARED_FONT_COUNT]={0,1,2,3,4,5};
 static const int zh_hans_order[SHARED_FONT_COUNT]={1,2,0,3,4,5};
 const int *order=NULL;
 if(language==shared_font_language("en-US"))order=en_us_order;
 else if(language==shared_font_language("zh-Hans"))order=zh_hans_order;
 else runtime_fail("shared font order is not verified for this language");
 if(max>0&&!out)runtime_fail("shared font data pointer is null");
 pthread_once(&shared_font_order_notice_once,log_shared_font_order_notice);
 pthread_once(&once,load);
 *count=SHARED_FONT_COUNT;
 if(max<=0)return 1;
 int limit=max<SHARED_FONT_COUNT?max:SHARED_FONT_COUNT;
 for(int i=0;i<limit;i++){
  int type=order[i];
  memset(&out[i],0,sizeof out[i]);out[i].type=(uint32_t)type;out[i].address=address(type);out[i].size=size(type);
 }
 return 1;
}
#endif
#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md): it asks for each font to be loaded before reading it; the fonts are loaded at
   startup. 1.6.0 also asks whether Labo VR mode is on: never on a Mac. */
static void request_load(int type){(void)type;}
static unsigned char vr_mode(void){return 0;}
static void vr_mode_change(void){fprintf(stderr,"[botw] Labo VR mode is not available\n");}
#endif
void *runtime_font_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
  {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",load_state},
  {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",address},
  {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",size}
#ifdef GAME_BOTW
  ,{"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE",request_load},{"_ZN2nn2pl8IsVrModeEv",vr_mode},
  {"_ZN2nn2pl11BeginVrModeEv",vr_mode_change},{"_ZN2nn2pl9EndVrModeEv",vr_mode_change}
#endif
#ifdef GAME_ACNH
  ,{"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE",request_load_acnh},
  {"_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE",get_shared_font}
#endif
  };
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
