#undef NDEBUG
#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef int32_t (ABI *Module)(uint16_t);
typedef int32_t (ABI *Init)(const void *,void *);
typedef int32_t (ABI *Param)(uint32_t,int32_t *);
typedef int32_t (ABI *List)(uint32_t,void *,uint32_t,uint32_t *);
typedef int32_t (ABI *Addon)(uint32_t,const void *,void *);
#define GET(t,n) ((t)runtime_content_resolve(n))
static void unexpected_cleanup(void) { fputs("unexpected fatal cleanup\n",stderr); abort(); }
#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_dir(path) mkdir(path,0755)
#endif
/* runtime_file.c is not linked here: record what the add-on provider mounts (#39). */
static char mounted_guest[4][64], mounted_host[4][512];
static unsigned mount_calls, unmount_calls;
int runtime_file_mount(const char *guest,const char *host) {
    assert(mount_calls<4);
    snprintf(mounted_guest[mount_calls],64,"%s",guest);
    snprintf(mounted_host[mount_calls],512,"%s",host);
    ++mount_calls; return 0;
}
void runtime_file_unmount(const char *guest) { (void)guest; ++unmount_calls; }
/* A minimal PSF (version 1.1) with UTF-8 strings, like an extracted add-on's param.sfo. */
static void write_sfo(const char *dir,const char *category,const char *content_id,const char *title) {
    char path[600]; snprintf(path,sizeof(path),"%s/sce_sys",dir); make_dir(path);
    snprintf(path,sizeof(path),"%s/sce_sys/param.sfo",dir);
    const char *keys[3]={"CATEGORY","CONTENT_ID","TITLE"}, *values[3]={category,content_id,title};
    unsigned char key_table[64]={0}, data[256]={0}, entries[3*16];
    uint32_t key_size=0, data_size=0;
    for (int i=0;i<3;++i) {
        uint16_t key_offset=(uint16_t)key_size, format=0x0204;
        uint32_t length=(uint32_t)strlen(values[i])+1, maximum=(length+3)&~3u, offset=data_size;
        memcpy(key_table+key_size,keys[i],strlen(keys[i])+1); key_size+=(uint32_t)strlen(keys[i])+1;
        memcpy(data+data_size,values[i],length); data_size+=maximum;
        memcpy(entries+i*16,&key_offset,2); memcpy(entries+i*16+2,&format,2); memcpy(entries+i*16+4,&length,4);
        memcpy(entries+i*16+8,&maximum,4); memcpy(entries+i*16+12,&offset,4);
    }
    key_size=(key_size+3)&~3u;
    uint32_t version=0x101, key_start=20+3*16, data_start=key_start+key_size, count=3;
    FILE *f=fopen(path,"wb"); assert(f);
    fwrite("\0PSF",1,4,f); fwrite(&version,4,1,f); fwrite(&key_start,4,1,f); fwrite(&data_start,4,1,f); fwrite(&count,4,1,f);
    fwrite(entries,1,sizeof(entries),f); fwrite(key_table,1,key_size,f); fwrite(data,1,data_size,f);
    fclose(f);
}
/* Extracted DLC (#39): a folder of add-ons one wrapper level down is listed as installed, mounted
 * read-only at /addcontN on request, and keyed by the last 16 characters of its CONTENT_ID. */
static void addcont_test(List list,Addon mount,Addon key) {
    const char *root="out/content-addcont-test";
    char path[600];
    make_dir(root);
    snprintf(path,sizeof(path),"%s/CUSA00035",root); make_dir(path);
    snprintf(path,sizeof(path),"%s/CUSA00035/TO1886GAMEDLC100",root); make_dir(path);
    write_sfo(path,"ac","UP9000-CUSA00035_00-TO1886GAMEDLC100","The Knight's Arsenal DLC");
    snprintf(path,sizeof(path),"%s/CUSA00035/TO1886GAMEDLC200",root); make_dir(path);
    write_sfo(path,"ac","UP9000-CUSA00035_00-TO1886GAMEDLC200","The Knight's Endurance DLC");
    snprintf(path,sizeof(path),"%s/CUSA00035/notdlc",root); make_dir(path);
    write_sfo(path,"gd","UP9000-CUSA00035_00-THEORDER18860000","not an add-on");
    runtime_content_scan(root);
    uint32_t hits=0;
    assert(list(0,NULL,0,&hits)==0 && hits==2);
    struct { char label[17]; char padding[3]; uint32_t status; } info[3];
    memset(info,0xee,sizeof(info));
    assert(list(0,info,3,&hits)==0 && hits==2);
    char seen[2][17]={{0}};
    for (int i=0;i<2;++i) { assert(info[i].status==4 && !info[i].label[16]); memcpy(seen[i],info[i].label,17); }
    assert((!strcmp(seen[0],"TO1886GAMEDLC100") && !strcmp(seen[1],"TO1886GAMEDLC200")) ||
           (!strcmp(seen[1],"TO1886GAMEDLC100") && !strcmp(seen[0],"TO1886GAMEDLC200")));
    assert((unsigned char)info[2].label[0]==0xee); /* the third slot is untouched */
    assert(list(0,info,1,&hits)==0 && hits==1);     /* capacity caps the listing */
    unsigned char label[20]={0}, point[16], key_out[16];
    memcpy(label,"TO1886GAMEDLC200",16);
    memset(point,0xcc,sizeof(point));
    assert(mount(0,label,point)==0 && !strcmp((char *)point,"/addcont0"));
    assert(mount_calls==1 && !strcmp(mounted_guest[0],"/addcont0") && strstr(mounted_host[0],"TO1886GAMEDLC200"));
    assert(mount(0,label,point)==0 && mount_calls==1); /* mounting again reuses the point */
    memcpy(label,"TO1886GAMEDLC100",16);
    assert(mount(0,label,point)==0 && !strcmp((char *)point,"/addcont1") && mount_calls==2);
    memset(key_out,0xcc,sizeof(key_out));
    assert(key(0,label,key_out)==0);
    for (unsigned i=0;i<sizeof(key_out);++i) assert(key_out[i]==0);
    memcpy(label,"THEORDER18860000",16);
    assert((uint32_t)mount(0,label,point)==0x80d90005 && (uint32_t)key(0,label,key_out)==0x80d90007);
    puts("PASS: extracted add-on content listed, mounted at /addcontN and keyed by entitlement label");
}
int main(int argc,char **argv) {
    Module load=GET(Module,"g8cM39EUZ6o#libSceSysmodule");
    Module loaded=GET(Module,"fMP5NHUOaMk#libSceSysmodule");
    Module unload=GET(Module,"eR2bZFAAU0Q#libSceSysmodule");
    Init init=GET(Init,"R9lA82OraNs#libSceAppContent");
    Param param=GET(Param,"99b82IKXpH4#libSceAppContent");
    List list=GET(List,"xnd8BJzAxmk#libSceAppContent");
    Addon mount=GET(Addon,"VANhIWcqYak#libSceAppContent"), key=GET(Addon,"XTWR0UXvcgs#libSceAppContent");
    assert(load && loaded && unload && init && param && list && mount && key);
    assert(!runtime_content_resolve("g8cM39EUZ6o#libkernel"));
    if (argc>2 && !strcmp(argv[1],"--sfo")) {
        if (!runtime_content_configure_sfo(argv[2])) { fputs("Rejected SFO metadata\n",stderr); return 2; }
        unsigned char text[9]; memset(text,0xa5,sizeof(text));
        assert(runtime_sfo_value(argv[2],"TITLE_ID",(char *)text,5,NULL) && memchr(text,0,5));
        for (unsigned i=5;i<sizeof(text);++i) assert(text[i]==0xa5);
        for (unsigned i=0;i<5;++i) { int32_t value; assert(param(i,&value)==0); }
        puts("PASS: AppContent SFO metadata"); return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--missing")) {
        unsigned char initial[32]={0},boot[40]; int32_t value=99;
        assert(load(0xb4)==0 && (uint32_t)init(initial,boot)==0x80d90002);
        assert((uint32_t)param(0,&value)==0x80d90002 && value==99);
        assert(unload(0xb4)==0);
        puts("PASS: absent metadata returns parameter error without a process stop"); return 0;
    }
    const uint32_t profile[]={3,13,0x80000000,0,7};
    runtime_content_configure(profile);
    if (argc>1 && !strcmp(argv[1],"--late")) {
        assert(atexit(unexpected_cleanup)==0 && load(0xb4)==0);
        runtime_content_configure(profile); return 99;
    }
    int32_t early=0; assert(param(1,&early)==0 && early==13);
    if (argc>1 && !strcmp(argv[1],"--unknown")) {
        /* Other system modules are host-provided: reference counted, independent. */
        assert((uint32_t)loaded(0xb5)==0x805a1001);
        assert(load(0xb5)==0 && loaded(0xb5)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
        assert(unload(0xb5)==0 && (uint32_t)loaded(0xb5)==0x805a1001);
        puts("PASS: independent module references"); return 0;
    }
    assert((uint32_t)loaded(0)==0x805a1000);
    assert((uint32_t)loaded(0xb4)==0x805a1001);
    assert((uint32_t)unload(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && load(0xb4)==0 && loaded(0xb4)==0);
    unsigned char initial[32]={0};
    struct { unsigned char boot[40]; uint32_t canary; } b;
    memset(&b,0xa5,sizeof(b));
    assert((uint32_t)init(NULL,b.boot)==0x80d90002);
    initial[31]=1; assert((uint32_t)init(initial,b.boot)==0x80d90002); initial[31]=0;
    assert(b.boot[0]==0xa5 && init(initial,b.boot)==0);
    for (unsigned i=0;i<40;++i) assert(b.boot[i]==0);
    assert(b.canary==0xa5a5a5a5);
    b.boot[0]=0xcc; assert((uint32_t)init(initial,b.boot)==0x80d90003 && b.boot[0]==0xcc);
    struct { int32_t value; uint32_t canary; } result={-1,0x1234};
    for (unsigned i=0;i<5;++i) {
        assert(param(i,&result.value)==0 && (uint32_t)result.value==profile[i]);
        assert(result.canary==0x1234);
    }
    result.value=77;
    assert((uint32_t)param(5,&result.value)==0x80d90002 && result.value==77);
    assert((uint32_t)param(0,NULL)==0x80d90002);
    unsigned char entries[48]; memset(entries,0xaa,sizeof(entries));
    struct { uint32_t hits,canary; } h={99,0xabcdef};
    assert(list(0,NULL,0,&h.hits)==0 && h.hits==0 && h.canary==0xabcdef);
    assert(list(0,entries,2,&h.hits)==0 && h.hits==0);
    for (unsigned i=0;i<sizeof(entries);++i) assert(entries[i]==0xaa);
    assert((uint32_t)list(0,NULL,0,NULL)==0x80d90002);
    assert(list(7,NULL,0,&h.hits)==0 && h.hits==0);
    unsigned char label[20]={0},addon_output[16]; memset(addon_output,0xcc,sizeof(addon_output));
    assert((uint32_t)mount(0,NULL,addon_output)==0x80d90002);
    assert((uint32_t)mount(0,label,NULL)==0x80d90002);
    assert((uint32_t)mount(0,label,addon_output)==0x80d90005);
    assert((uint32_t)key(0,NULL,addon_output)==0x80d90002);
    assert((uint32_t)key(0,label,NULL)==0x80d90002);
    assert((uint32_t)key(0,label,addon_output)==0x80d90007);
    for (unsigned i=0;i<sizeof(addon_output);++i) assert(addon_output[i]==0xcc);
    assert(unload(0xb4)==0 && loaded(0xb4)==0);
    assert((uint32_t)init(initial,b.boot)==0x80d90003);
    assert(unload(0xb4)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && init(initial,b.boot)==0 && unload(0xb4)==0);
    puts("PASS: AppContent lifecycle, profile and output boundaries");
    addcont_test(list,mount,key);
    typedef int32_t (ABI *Unmount)(void *);
    Unmount unmount=GET(Unmount,"3rHWaV-1KC4#libSceAppContent");
    char point[16]="/addcont0";
    assert(load(0xb4)==0 && init(initial,b.boot)==0 && unmount(point)==0 && unmount_calls==1 && unload(0xb4)==0);
    return 0;
}
