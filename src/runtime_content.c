/* Offline AppContent metadata from the application's param.sfo, plus extracted add-on content
 * (DLC folders, #39): each folder with sce_sys/param.sfo is listed as installed and mounted
 * read-only at /addcontN when the game asks for it. No downloads, licenses or entitlement
 * checks: what the player points the launcher at is what the game sees. */
#include "runtime.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define NOT_INITIALIZED ((int32_t)0x80d90001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
#define NOT_FOUND ((int32_t)0x80d90005)
#define NO_ENTITLEMENT ((int32_t)0x80d90007)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
/* Add-on content found by runtime_content_scan. The entitlement label is the last 16 characters
 * of the add-on's CONTENT_ID (UP9000-CUSA00035_00-TO1886GAMEDLC100 -> TO1886GAMEDLC100). */
#define MAX_ADDONS 16
typedef struct { char label[17]; char title[128]; char host[512]; char point[16]; } Addon;
static Addon addons[MAX_ADDONS];
static unsigned addon_count, mounts_made;
static void stop(const char *message,int status) {
    fputs(message,stderr); fflush(NULL); _Exit(status);
}
/* PSF tables and strings are bounded before any offset or typed value is read. */
int runtime_sfo_value(const char *path,const char *key,char *text,size_t text_size,uint32_t *number) {
    if (!path || !key) return 0;
    FILE *file=fopen(path,"rb");
    if (!file) return 0;
    unsigned char data[65536];
    size_t size=fread(data,1,sizeof(data),file);
    int extra=fgetc(file), failed=ferror(file);
    fclose(file);
    if (failed || extra!=EOF || size<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t version,keys,values,count;
    memcpy(&version,data+4,4); memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    if (version!=0x101 || count>(size-20)/16 || keys<20+(size_t)count*16 || keys>values || values>size) return 0;
    const unsigned char *selected=NULL;
    uint16_t selected_format=0; uint32_t selected_length=0;
    for (uint32_t i=0;i<count;++i) {
        const unsigned char *entry=data+20+(size_t)i*16;
        uint16_t key_offset,format; uint32_t length,maximum,offset;
        memcpy(&key_offset,entry,2); memcpy(&format,entry+2,2); memcpy(&length,entry+4,4);
        memcpy(&maximum,entry+8,4); memcpy(&offset,entry+12,4);
        if (key_offset>=values-keys || !memchr(data+keys+key_offset,0,values-keys-key_offset) ||
            offset>size-values || length>maximum || maximum>size-values-offset) return 0;
        if (strcmp((const char *)data+keys+key_offset,key)) continue;
        if (selected) return 0;
        selected=data+values+offset; selected_format=format; selected_length=length;
    }
    if (!selected) return 0;
    if (selected_format==0x0404 && number && selected_length==4) { memcpy(number,selected,4); return 1; }
    if (selected_format==0x0204 && text && text_size && selected_length && memchr(selected,0,selected_length)) {
        size_t copy=strlen((const char *)selected);
        if (copy>=text_size) copy=text_size-1;
        memcpy(text,selected,copy); text[copy]=0; return 1;
    }
    return 0;
}
void runtime_content_configure(const uint32_t values[5]) {
    if (!values || refs || initialized || (values[0]!=1 && values[0]!=3))
        stop("ERROR: invalid or late AppContent metadata\n",1);
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
int runtime_content_configure_sfo(const char *path) {
    char title[16];
    if (!runtime_sfo_value(path,"TITLE_ID",title,sizeof(title),NULL) || !title[0]) return 0;
    /* The supplied shadPS4 AppContent implementation returns FULL=3 for SKU;
     * this compatibility value is not a license or entitlement determination. */
    uint32_t values[5]={3,0,0,0,0};
    for (unsigned i=1;i<5;++i) {
        char key[32]; snprintf(key,sizeof(key),"USER_DEFINED_PARAM_%u",i);
        runtime_sfo_value(path,key,NULL,0,&values[i]);
    }
    runtime_content_configure(values);
    printf("Runtime: AppContent SFO %s: SKU=3 (reference compatibility value), user parameters=%u,%u,%u,%u\n",
           title,values[1],values[2],values[3],values[4]);
    return 1;
}
static void add_addon(const char *folder) {
    char sfo[700], content[64]="", category[8]="";
    if (snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",folder)>=(int)sizeof(sfo)) return;
    if (!runtime_sfo_value(sfo,"CONTENT_ID",content,sizeof(content),NULL)) return;
    runtime_sfo_value(sfo,"CATEGORY",category,sizeof(category),NULL);
    const size_t n=strlen(content);
    if (n<16 || (category[0] && strcmp(category,"ac"))) {
        printf("Runtime: %s is not add-on content (CONTENT_ID %s, CATEGORY %s); skipped\n",folder,content,category);
        return;
    }
    const char *label=content+n-16;
    for (unsigned i=0;i<addon_count;++i) if (!strcmp(addons[i].label,label)) {
        printf("Runtime: add-on %s found twice; keeping %s\n",label,addons[i].host);
        return;
    }
    if (addon_count==MAX_ADDONS || strlen(folder)>=sizeof(addons[0].host)) {
        printf("Runtime: add-on %s skipped (too many add-ons or path too long)\n",label);
        return;
    }
    Addon *a=&addons[addon_count++];
    memcpy(a->label,label,16); a->label[16]=0;
    if (!runtime_sfo_value(sfo,"TITLE",a->title,sizeof(a->title),NULL)) snprintf(a->title,sizeof(a->title),"%s",label);
    snprintf(a->host,sizeof(a->host),"%s",folder);
    printf("Runtime: add-on %s \"%s\" from %s\n",a->label,a->title,a->host);
}
/* A DLC folder: either one extracted add-on (sce_sys/param.sfo directly inside) or a folder of
 * them, possibly one level deeper (DLC/CUSA00035/TO1886GAMEDLC100). */
/* Paths longer than an add-on's host field (511 characters) are skipped, never truncated. */
#define ADDON_PATH 512
static int join(char *out,const char *a,const char *b) {
    const int n=snprintf(out,ADDON_PATH,"%s/%s",a,b);
    return n>0 && n<ADDON_PATH;
}
void runtime_content_scan(const char *folder) {
    if (!folder || !*folder) return;
    char path[ADDON_PATH], current[ADDON_PATH], child[ADDON_PATH], only[ADDON_PATH];
    struct stat st;
    if (strlen(folder)>=ADDON_PATH) { printf("Runtime: add-on folder path is too long; no add-ons\n"); return; }
    snprintf(current,sizeof(current),"%s",folder);
    if (join(path,current,"sce_sys/param.sfo") && !stat(path,&st)) { add_addon(current); return; }
    for (int depth=0;depth<2;++depth) {
        const unsigned before=addon_count;
        DIR *d=opendir(current);
        if (!d) { printf("Runtime: add-on folder %s cannot be read; no add-ons\n",current); return; }
        unsigned dirs=0; only[0]=0;
        struct dirent *e;
        while ((e=readdir(d))) {
            if (e->d_name[0]=='.' || !join(child,current,e->d_name)) continue;
            if (stat(child,&st) || !S_ISDIR(st.st_mode)) continue;
            ++dirs; memcpy(only,child,sizeof(only));
            if (join(path,child,"sce_sys/param.sfo") && !stat(path,&st)) add_addon(child);
        }
        closedir(d);
        if (addon_count>before || depth==1 || dirs!=1) break;
        memcpy(current,only,sizeof(current)); /* one wrapper folder (the title ID): look inside it */
    }
    if (!addon_count) printf("Runtime: no add-on content found in the DLC folder\n");
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (module_refs[id]==UINT32_MAX) stop("STOP: module reference overflow\n",21);
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    if (!configured || !init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* The public init structure contains only reserved bytes. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    printf("Runtime: AppContent initialized from application metadata; %u add-on(s) available\n",addon_count);
    return 0;
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    if (!configured || id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
/* SceAppContentAddcontInfo: entitlement label (17 bytes + 3 padding) and download status. */
typedef struct { char label[17]; char padding[3]; uint32_t status; } AddcontInfo;
_Static_assert(sizeof(AddcontInfo)==24,"SceAppContentAddcontInfo layout");
#define ADDCONT_INSTALLED 4u
static const Addon *find_addon(const void *label) {
    const char *text=(const char *)label;
    for (unsigned i=0;i<addon_count;++i) if (!memcmp(addons[i].label,text,16)) return &addons[i];
    return NULL;
}
/* sceAppContentGetAddcontInfoList: every add-on is installed. Without a list it counts them. */
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    (void)service;
    if (!configured) return PARAMETER;
    if ((!capacity || !list) && !hits) return PARAMETER;
    uint32_t listed=addon_count;
    if (list && capacity) {
        if (listed>capacity) listed=capacity;
        AddcontInfo *out=list;
        for (uint32_t i=0;i<listed;++i) {
            memset(&out[i],0,sizeof(out[i]));
            memcpy(out[i].label,addons[i].label,16);
            out[i].status=ADDCONT_INSTALLED;
        }
    }
    if (hits) *hits=listed;
    if (!lists++) printf("Runtime: AppContent add-on list: %u add-on(s)\n",addon_count);
    return 0;
}
/* sceAppContentAddcontMount: /addcontN, read-only (runtime_file.c treats /addcont* as game data). */
static ABI int32_t addon_mount(uint32_t service,const void *label,void *mount_point) {
    (void)service;
    if (!configured || !label || !mount_point) return PARAMETER;
    Addon *a=(Addon *)find_addon(label);
    if (!a) return NOT_FOUND;
    if (!a->point[0]) {
        snprintf(a->point,sizeof(a->point),"/addcont%u",mounts_made++);
        if (runtime_file_mount(a->point,a->host)) { a->point[0]=0; return BUSY; }
        printf("Runtime: add-on %s mounted at %s\n",a->label,a->point);
    }
    memset(mount_point,0,16);
    memcpy(mount_point,a->point,strlen(a->point));
    return 0;
}
/* sceAppContentGetEntitlementKey: extracted add-ons need no decryption key; a known label gets a
 * zero key, as the reference implementation returns. */
static ABI int32_t entitlement_key(uint32_t service,const void *label,void *key) {
    (void)service;
    if (!configured || !label || !key) return PARAMETER;
    if (!find_addon(label)) return NO_ENTITLEMENT;
    memset(key,0,16);
    return 0;
}
/* sceAppContentAddcontUnmount: releases a mount point made above; an unknown point is a no-op,
 * as in upstream shadPS4. */
static ABI int32_t addon_unmount(void *mount_point) {
    if (!initialized) return NOT_INITIALIZED;
    if (!mount_point) return PARAMETER;
    char point[17]; memcpy(point,mount_point,16); point[16]=0;
    for (unsigned i=0;i<addon_count;++i) if (addons[i].point[0] && !strcmp(addons[i].point,point)) {
        runtime_file_unmount(point);
        printf("Runtime: add-on %s unmounted from %s\n",addons[i].label,point);
        addons[i].point[0]=0;
    }
    return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    if (!strcmp(name,"VANhIWcqYak#libSceAppContent")) return (uintptr_t)addon_mount;
    if (!strcmp(name,"3rHWaV-1KC4#libSceAppContent")) return (uintptr_t)addon_unmount;
    if (!strcmp(name,"XTWR0UXvcgs#libSceAppContent")) return (uintptr_t)entitlement_key;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u, add-ons %u (mounted %u)\n",refs,initialized,queries,lists,addon_count,mounts_made);
}
