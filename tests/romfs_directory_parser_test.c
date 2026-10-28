/* Synthetic metadata regression for the exact production RomFS parser and directory catalog. */
#include "../engine/runtime/romfs.c"
#include <assert.h>
#include <setjmp.h>

static jmp_buf failure;
static char fail_message[128];
_Noreturn void runtime_fail(const char *message) { snprintf(fail_message,sizeof fail_message,"%s",message);longjmp(failure,1); }
void runtime_boot_progress(const char *stage,uint64_t done,uint64_t total) { (void)stage;(void)done;(void)total; }

static size_t align4(size_t n) { return (n + 3) & ~(size_t)3; }
static size_t put_dir(uint8_t *table,size_t off,uint32_t parent,uint32_t sibling,uint32_t child,uint32_t file,const char *name) {
    uint32_t m[6]={parent,sibling,child,file,UINT32_MAX,(uint32_t)strlen(name)};memcpy(table+off,m,sizeof m);memcpy(table+off+24,name,strlen(name));return align4(off+24+strlen(name));
}
static size_t put_file(uint8_t *table,size_t off,uint32_t parent,uint32_t sibling,uint64_t data,uint64_t size,const char *name) {
    uint32_t m[8]={parent,sibling};memcpy(m+2,&data,8);memcpy(m+4,&size,8);m[6]=UINT32_MAX;m[7]=(uint32_t)strlen(name);memcpy(table+off,m,sizeof m);memcpy(table+off+32,name,strlen(name));return align4(off+32+strlen(name));
}
static void test_synthetic(void) {
    uint8_t dirs[256]={0},files[256]={0};
    size_t dend=put_dir(dirs,0,0,UINT32_MAX,24,UINT32_MAX,"");dend=put_dir(dirs,dend,0,120,56,0,"Rumble");size_t empty=56;
    dend=put_dir(dirs,empty,24,88,UINT32_MAX,UINT32_MAX,"Empty");
    dend=put_dir(dirs,dend,24,UINT32_MAX,UINT32_MAX,40,"Nested");
    dend=put_dir(dirs,120,0,UINT32_MAX,UINT32_MAX,80,"Other");
    size_t fend=0;fend=put_file(files,fend,24,UINT32_MAX,0,7,"a.bnvi");
    fend=put_file(files,fend,88,UINT32_MAX,7,5,"\xce\xb2.bin");
    fend=put_file(files,fend,120,UINT32_MAX,12,2,"top.bin");
    uint64_t dataoff=512, foff=80+dend;uint8_t header[80]={0};uint64_t h[10]={80,80,0,80,dend,80+dend,0,foff,fend,dataoff};memcpy(header,h,sizeof h);
    char image_path[]="/tmp/romfs-dir-test-XXXXXX";image_fd=mkstemp(image_path);assert(image_fd>=0);unlink(image_path);
    assert(pwrite(image_fd,header,sizeof header,0)==sizeof header);assert(pwrite(image_fd,dirs,dend,80)==(ssize_t)dend);assert(pwrite(image_fd,files,fend,(off_t)foff)==(ssize_t)fend);
    const char *paths[]={"Other/top.bin","Rumble/Nested/\xce\xb2.bin","Rumble/a.bnvi"};uint64_t offsets[]={dataoff+12,dataoff+7,dataoff};uint64_t sizes[]={2,5,7};
    file_count=3;strings_offset=24+file_count*24;index_size=(size_t)strings_offset+128;index_bytes=calloc(index_size,1);assert(index_bytes);memcpy(index_bytes,"TOTKRFS1",8);memcpy(index_bytes+8,&file_count,8);memcpy(index_bytes+16,&strings_offset,8);
    size_t names=0;for(size_t i=0;i<3;i++){Entry e={offsets[i],sizes[i],(uint32_t)names,(uint32_t)strlen(paths[i])+1};memcpy(index_bytes+24+i*24,&e,sizeof e);memcpy(index_bytes+strings_offset+names,paths[i],e.len);names+=e.len;}
    if(!setjmp(failure))verify_dir_metadata();else assert(!"valid synthetic RomFS metadata rejected");
    RomfsDirFile catalog_files[3];for(size_t i=0;i<3;i++)catalog_files[i]=(RomfsDirFile){paths[i],sizes[i]};
    directory_index=romfs_dir_index_create((const char *const *)romfs_dirs,romfs_dir_count,catalog_files,3);assert(directory_index);
    assert(romfs_dir_index_directory_count(directory_index,"Rumble")==2&&romfs_dir_index_file_count(directory_index,"Rumble")==1);
    assert(romfs_dir_index_directory_count(directory_index,"Rumble/Empty")==0);
    assert(!romfs_dir_open(directory_index,"Rum",3)&&!romfs_dir_open(directory_index,"Missing",3));
    RomfsDirCursor *cursor=romfs_dir_open(directory_index,"Rumble",3);assert(cursor);RomfsDirectoryEntry out[4];int64_t n=-1;
    assert(!romfs_dir_read(cursor,&n,out,0)&&n==0);
    assert(!romfs_dir_read(cursor,&n,out,1)&&n==1&&out[0].type==0&&!strcmp(out[0].name,"Empty"));
    assert(!romfs_dir_read(cursor,&n,out,4)&&n==2&&out[1].type==1&&out[1].size==7&&!strcmp(out[1].name,"a.bnvi"));
    assert(!romfs_dir_read(cursor,&n,out,4)&&n==0);romfs_dir_close(cursor);
    cursor=romfs_dir_open(directory_index,"Rumble",ROMFS_DIR_READ_FILES);assert(cursor);
    assert(!romfs_dir_read(cursor,&n,out,4)&&n==1&&out[0].type==1&&!strcmp(out[0].name,"a.bnvi"));romfs_dir_close(cursor);
    cursor=romfs_dir_open(directory_index,"Rumble/Nested",ROMFS_DIR_READ_FILES|ROMFS_DIR_NO_FILE_SIZE);assert(cursor);
    assert(!romfs_dir_read(cursor,&n,out,4)&&n==1&&!strcmp(out[0].name,"\xce\xb2.bin")&&out[0].size==0);romfs_dir_close(cursor);
    cursor=romfs_dir_open(directory_index,"",ROMFS_DIR_READ_DIRS);assert(cursor);
    assert(!romfs_dir_read(cursor,&n,out,1)&&n==1&&!strcmp(out[0].name,"Other"));
    assert(!romfs_dir_read(cursor,&n,out,4)&&n==1&&!strcmp(out[0].name,"Rumble"));romfs_dir_close(cursor);
    char *long_name=malloc(770);assert(long_name);memset(long_name,'x',769);long_name[769]=0;
    const char *long_dirs[]={""};RomfsDirFile boundary={long_name,1};
    assert(!romfs_dir_index_create(long_dirs,1,&boundary,1));long_name[768]=0;RomfsDirFile accepted={long_name,1};
    RomfsDirIndex *boundary_index=romfs_dir_index_create(long_dirs,1,&accepted,1);assert(boundary_index);romfs_dir_index_destroy(boundary_index);free(long_name);
    assert(romfs_dir_open(directory_index,"",0)==NULL&&romfs_dir_open(directory_index,"",4)==NULL);
    romfs_dir_index_destroy(directory_index);directory_index=NULL;
    /* Two distinct metadata nodes may not claim the same runtime-index path. */
    uint64_t meta[10];memcpy(meta,header,sizeof meta);uint32_t sibling=40,parent=24,name_len=6,empty_file=UINT32_MAX;uint64_t duplicate_data=0,duplicate_size=7;
    assert(pwrite(image_fd,&sibling,4,(off_t)meta[7]+4)==4);assert(pwrite(image_fd,&parent,4,(off_t)meta[7]+40)==4);
    assert(pwrite(image_fd,&duplicate_data,8,(off_t)meta[7]+40+8)==8);assert(pwrite(image_fd,&duplicate_size,8,(off_t)meta[7]+40+16)==8);
    assert(pwrite(image_fd,&name_len,4,(off_t)meta[7]+40+28)==4);assert(pwrite(image_fd,"a.bnvi",6,(off_t)meta[7]+40+32)==6);
    assert(pwrite(image_fd,&empty_file,4,80+88+12)==4);
    for(size_t i=0;i<romfs_dir_count;i++)free(romfs_dirs[i]);free(romfs_dirs);romfs_dirs=NULL;romfs_dir_count=romfs_dir_cap=0;fail_message[0]=0;
    if(!setjmp(failure)){verify_dir_metadata();assert(!"duplicate runtime-index path accepted");}
    assert(!strcmp(fail_message,"RomFS file metadata/index mismatch"));
    for(size_t i=0;i<romfs_dir_count;i++)free(romfs_dirs[i]);free(romfs_dirs);romfs_dirs=NULL;romfs_dir_count=romfs_dir_cap=0;
    assert(pwrite(image_fd,files,fend,(off_t)meta[7])==(ssize_t)fend);assert(pwrite(image_fd,dirs,dend,80)==(ssize_t)dend);
    uint32_t original_sibling=UINT32_MAX,original_parent=24;uint64_t original_data=0;
    uint32_t cycle=0;assert(pwrite(image_fd,&cycle,4,(off_t)meta[7]+4)==4);fail_message[0]=0;
    if(!setjmp(failure)){verify_dir_metadata();assert(!"file sibling cycle accepted");}
    assert(!strcmp(fail_message,"RomFS file sibling cycle or offset"));
    for(size_t i=0;i<romfs_dir_count;i++)free(romfs_dirs[i]);free(romfs_dirs);romfs_dirs=NULL;romfs_dir_count=romfs_dir_cap=0;
    assert(pwrite(image_fd,&original_sibling,4,(off_t)meta[7]+4)==4);
    uint32_t bad_parent=0;assert(pwrite(image_fd,&bad_parent,4,(off_t)meta[7])==4);fail_message[0]=0;
    if(!setjmp(failure)){verify_dir_metadata();assert(!"wrong file parent accepted");}
    assert(!strcmp(fail_message,"RomFS file parent or bounds"));
    for(size_t i=0;i<romfs_dir_count;i++)free(romfs_dirs[i]);free(romfs_dirs);romfs_dirs=NULL;romfs_dir_count=romfs_dir_cap=0;
    assert(pwrite(image_fd,&original_parent,4,(off_t)meta[7])==4);
    uint64_t bad_data=(1ULL<<40);assert(pwrite(image_fd,&bad_data,8,(off_t)meta[7]+8)==8);fail_message[0]=0;
    if(!setjmp(failure)){verify_dir_metadata();assert(!"out-of-bounds file data accepted");}
    assert(!strcmp(fail_message,"RomFS file parent or bounds"));
    assert(pwrite(image_fd,&original_data,8,(off_t)meta[7]+8)==8);
    for(size_t i=0;i<romfs_dir_count;i++)free(romfs_dirs[i]);free(romfs_dirs);romfs_dirs=NULL;romfs_dir_count=romfs_dir_cap=0;
    close(image_fd);image_fd=-1;free(index_bytes);index_bytes=NULL;
}
static int read_file(const char *path,uint8_t **out,size_t *size) {
    int fd=open(path,O_RDONLY);if(fd<0)return 0;struct stat st;if(fstat(fd,&st)||st.st_size<0||(uint64_t)st.st_size>SIZE_MAX){close(fd);return 0;}
    *size=(size_t)st.st_size;*out=malloc(*size);int ok=*out&&read(fd,*out,*size)==(ssize_t)*size;close(fd);return ok;
}
static void test_real(const char *image,const char *index_path) {
    uint8_t *idx=NULL;size_t idx_size=0;assert(read_file(index_path,&idx,&idx_size));index_bytes=idx;index_size=idx_size;
    assert(index_size>=24&&!memcmp(index_bytes,"TOTKRFS1",8));memcpy(&file_count,index_bytes+8,8);memcpy(&strings_offset,index_bytes+16,8);
    assert(strings_offset==24+file_count*24);int fd=open(image,O_RDONLY);assert(fd>=0);image_fd=fd;
    if(!setjmp(failure)){verify_dir_metadata();}else assert(!"real RomFS verification/parser failed");
    RomfsDirFile *catalog=calloc(file_count,sizeof(*catalog));assert(catalog);for(uint64_t i=0;i<file_count;i++){Entry e=entry(i);catalog[i]=(RomfsDirFile){entry_name(e),e.size};}
    directory_index=romfs_dir_index_create((const char *const *)romfs_dirs,romfs_dir_count,catalog,file_count);assert(directory_index);
    size_t dirs_count=romfs_dir_index_directory_count(directory_index,"Rumble"),files_count=romfs_dir_index_file_count(directory_index,"Rumble");
    RomfsDirCursor *cursor=romfs_dir_open(directory_index,"Rumble",3);assert(cursor);RomfsDirectoryEntry out[64];int64_t count=0;assert(!romfs_dir_read(cursor,&count,out,64));romfs_dir_close(cursor);
    printf("{\"status\":\"pass\",\"romfs_directories\":%zu,\"romfs_files\":%llu,\"rumble_directories\":%zu,\"rumble_files\":%zu,\"rumble_returned\":%lld}\n",romfs_dir_count,(unsigned long long)file_count,dirs_count,files_count,(long long)count);
    fprintf(stderr,"real Rumble listing: dirs=%zu files=%zu returned=%lld\n",dirs_count,files_count,(long long)count);
    assert(count==33&&files_count==33&&dirs_count==0);free(catalog);romfs_dir_index_destroy(directory_index);free(index_bytes);close(image_fd);
}
int main(int argc,char **argv) { if(argc==1)test_synthetic();else {assert(argc==3);test_real(argv[1],argv[2]);}return 0; }
