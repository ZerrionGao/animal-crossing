/* Offline local user identity. Handles are validated; profile survives restarts. */
#include "runtime.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
typedef struct{uint64_t lo,hi;} Uid;
typedef struct{Uid uid;uint64_t token;} UserHandle;
static Uid local;
#ifdef GAME_ACNH
static Uid residents[2];
#endif
/* Canaries around the lock: an intermittent hang showed this mutex's internal words changed while nobody held it. */
#define GUARD 0x6163636f756e7447ULL
static struct{uint64_t pre[8];pthread_mutex_t m;uint64_t post[8];} guarded={{GUARD,GUARD,GUARD,GUARD,GUARD,GUARD,GUARD,GUARD},PTHREAD_MUTEX_INITIALIZER,{GUARD,GUARD,GUARD,GUARD,GUARD,GUARD,GUARD,GUARD}};
#define lock_ guarded.m
static _Atomic(uint64_t) lock_owner;
static void check_guard(void){for(unsigned i=0;i<8;i++)if(guarded.pre[i]!=GUARD||guarded.post[i]!=GUARD){fprintf(stderr,"[totk] account lock guard %s[%u] overwritten: %#llx\n",guarded.pre[i]!=GUARD?"pre":"post",i,(unsigned long long)(guarded.pre[i]!=GUARD?guarded.pre[i]:guarded.post[i]));runtime_fail("host memory near the account lock was overwritten");}}
static void lock_acquire(void){check_guard();pthread_mutex_lock(&lock_);uint64_t t;pthread_threadid_np(NULL,&t);lock_owner=t;}
static void lock_release(void){lock_owner=0;pthread_mutex_unlock(&lock_);check_guard();}
uint64_t runtime_account_lock_owner(void){const uint32_t *w=(const uint32_t *)(const void *)&guarded;fprintf(stderr,"[totk-dump] account lock with guards @%p:",(void *)&guarded);for(unsigned i=0;i<sizeof(guarded)/4;i++)fprintf(stderr," %08x",w[i]);fputc('\n',stderr);return lock_owner;}
static uint64_t tokens[64],nextToken=1;
static void initialize(void){
 lock_acquire();if(local.lo||local.hi){lock_release();return;}
 const char *dir=getenv("TOTK_PROFILE_DIR");if(!dir)dir="user-data";
 if(mkdir(dir,0700)&&errno!=EEXIST)runtime_fail("create local account directory");
 char path[4096];if(snprintf(path,sizeof(path),"%s/account.uid",dir)>=(int)sizeof(path))runtime_fail("local account path length");
 int fd=open(path,O_RDWR|O_CREAT|O_NOFOLLOW,0600);if(fd<0||flock(fd,LOCK_EX))runtime_fail("open local account identity");
 struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode))runtime_fail("invalid local account identity file");
 if(st.st_size==0){do{arc4random_buf(&local,sizeof(local));}while(!local.lo&&!local.hi);if(write(fd,&local,sizeof(local))!=sizeof(local)||fsync(fd))runtime_fail("persist local account identity");}
 else if(st.st_size!=sizeof(local)||pread(fd,&local,sizeof(local),0)!=sizeof(local)||(!local.lo&&!local.hi))runtime_fail("corrupt local account identity");
 close(fd);
#ifdef GAME_ACNH
 residents[0]=local;
 const char *selection=getenv("ACNH_USER"); int selected=selection && !strcmp(selection,"1");
 if(snprintf(path,sizeof(path),"%s/account2.uid",dir)>=(int)sizeof(path))runtime_fail("second account path length");
 fd=open(path,O_RDWR|O_NOFOLLOW|(selected?O_CREAT:0),0600);
 if(fd>=0){
  if(flock(fd,LOCK_EX)||fstat(fd,&st)||!S_ISREG(st.st_mode))runtime_fail("invalid second account identity file");
  if(st.st_size==0 && selected){do{arc4random_buf(&residents[1],sizeof(Uid));}while(!residents[1].lo&&!residents[1].hi);if(write(fd,&residents[1],sizeof(Uid))!=sizeof(Uid)||fsync(fd))runtime_fail("persist second account identity");}
  else if(st.st_size!=sizeof(Uid)||pread(fd,&residents[1],sizeof(Uid),0)!=sizeof(Uid)||(!residents[1].lo&&!residents[1].hi))runtime_fail("corrupt second account identity");
  close(fd);
 } else if(errno!=ENOENT || selected)runtime_fail("open second account identity");
 if(selected)local=residents[1];
#endif
 lock_release();fprintf(stderr,"[totk] persistent local account initialized\n");
}
static bool same(const Uid *uid){
 if(!uid || (!uid->lo && !uid->hi))return false;
#ifdef GAME_ACNH
 return (uid->lo==residents[0].lo && uid->hi==residents[0].hi)||(uid->lo==residents[1].lo && uid->hi==residents[1].hi);
#else
 return uid->lo==local.lo&&uid->hi==local.hi;
#endif
}
static bool valid(const UserHandle *handle){if(!handle||!same(&handle->uid))return false;for(unsigned i=0;i<64;i++)if(tokens[i]&&tokens[i]==handle->token)return true;return false;}
static unsigned preselected(UserHandle *out){if(!out)runtime_fail("account null user handle");initialize();lock_acquire();for(unsigned i=0;i<64;i++)if(!tokens[i]){tokens[i]=nextToken++;*out=(UserHandle){local,tokens[i]};lock_release();return 1;}lock_release();return 0;}
static uint32_t user_id(Uid *out,const UserHandle *handle){lock_acquire();bool ok=out&&valid(handle);if(ok)*out=local;lock_release();return ok?0:0x7c67;}
static void close_user(const UserHandle *handle){lock_acquire();if(!valid(handle))runtime_fail("invalid local account handle close");for(unsigned i=0;i<64;i++)if(tokens[i]==handle->token){tokens[i]=0;break;}lock_release();}
static uint32_t existence(unsigned char *out,const Uid *uid){if(!out||!uid)return 0x7c67;initialize();*out=same(uid);return 0;}
static uint32_t nickname(char *out,const Uid *uid){initialize();if(!out||!same(uid))return 0x7c67;memset(out,0,33);memcpy(out,"Local",5);return 0;}
void runtime_account_uid(uint64_t out[2]){initialize();out[0]=local.lo;out[1]=local.hi;}
int runtime_account_index(const uint64_t uid[2]){
 if(!uid || (!uid[0] && !uid[1]))return -1;
 initialize();
#ifdef GAME_ACNH
 for(int i=0;i<2;i++)if(uid[0]==residents[i].lo && uid[1]==residents[i].hi)return i;
#else
 if(uid[0]==local.lo && uid[1]==local.hi)return 0;
#endif
 return -1;
}
#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md) opens the preselected user with the aborting variant and lists the users. */
static void open_preselected(UserHandle *out){if(!preselected(out))runtime_fail("open preselected user");}
static uint32_t list_users(int *count,Uid *out,int max){initialize();if(!count||(max>0&&!out))return 0x7c67;*count=0;if(max>0){out[0]=local;*count=1;}return 0;}
#endif
#ifdef GAME_ACNH
/* 与原 SDK 一样写满 max 个槽位，空槽为无效 Uid（全 0）：读档时 main+0xcaea34 用未清零的 8 项栈数组接收，
   再对每个非零项调用 olsc 关闭云备份；只写第 0 项会把栈上残留当成其他用户。 */
static uint32_t list_users_acnh(int *count,Uid *out,int max){
 if(!count||(max>0&&!out))return 0x7c67;
 initialize();
 *count=(residents[1].lo||residents[1].hi)?2:1;
 for(int i=0;i<max;i++)out[i]=i<*count?residents[i]:(Uid){0,0};
 return 0;
}
/* 本地句柄在 +0x10 放的是序号，原 SDK 却把它当成网络账号对象。这些查询改为离线失败，避免空指针。 */
static uint32_t network_unavailable(void){
 static int said; if(!said){said=1;fprintf(stderr,"[acnh] 没有网络账号，继续离线\n");}
 return 0x7c67;
}
static uint32_t network_service_available(bool *out,const UserHandle *handle){(void)handle;if(out)*out=false;return network_unavailable();}
static uint32_t ensure_network_service(const UserHandle *handle){(void)handle;return network_unavailable();}
static uint32_t get_network_id(uint64_t *out,const UserHandle *handle){(void)handle;if(out)*out=0;return network_unavailable();}
static uint32_t load_network_token(unsigned long *n,char *buf,unsigned long cap,const UserHandle *handle){(void)handle;(void)buf;(void)cap;if(n)*n=0;return network_unavailable();}
static uint32_t ensure_network_token_async(void *async,const UserHandle *handle){(void)async;(void)handle;return network_unavailable();}
/* 好友库会列出已打开用户。原实现要向账号服务取名单，这里服务指针是空的。 */
static uint32_t list_open_users(int *count,Uid *out,int max){
 if(!count||(max>0&&!out))return 0x7c67;
 initialize();
 lock_acquire();
 int open=0;for(unsigned i=0;i<64;i++)if(tokens[i]){open=1;break;}
 lock_release();
 *count=open;for(int i=0;i<max;i++)out[i]=!i&&open?local:(Uid){0,0};
 static int said;if(!said){said=1;fprintf(stderr,"[acnh] 已打开用户 %d\n",open);}
 return 0;
}
#endif
void *runtime_account_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn7account10InitializeEv",initialize},{"_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE",preselected},
 {"_ZN2nn7account9CloseUserERKNS0_10UserHandleE",close_user},{"_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE",user_id},
 {"_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE",existence},{"_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE",nickname}
#ifdef GAME_BOTW
 ,{"_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE",open_preselected},{"_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi",list_users}
#endif
#ifdef GAME_ACNH
 ,{"_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi",list_users_acnh}
 ,{"_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi",list_open_users}
 ,{"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE",network_service_available}
 ,{"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE",ensure_network_service}
 ,{"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",get_network_id}
 ,{"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE",load_network_token}
 ,{"_ZN2nn7account42EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",ensure_network_token_async}
#endif
 };
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
