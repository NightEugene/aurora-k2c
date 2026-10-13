// Load the device's Android compatibility backend only when starting the game.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

static void *(*backend_open)(const char *, int);
static void *(*backend_symbol)(void *, const char *);
static char *(*backend_error)(void);
static void (*backend_hook)(void *(*)(const char *, const char *));
static void *(*backend_namespace)(const char *, const char *, const char *, uint64_t, const char *, void *);
static void *(*backend_open_ext)(const char *, int, const void *);

int k2c_hybris_init(void)
{
    void *library=dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_GLOBAL);
    if(!library){fprintf(stderr,"[launcher] libhybris unavailable: %s\n",dlerror());return 0;}
    backend_open=dlsym(library,"hybris_dlopen");
    backend_symbol=dlsym(library,"hybris_dlsym");
    backend_error=dlsym(library,"hybris_dlerror");
    backend_hook=dlsym(library,"hybris_set_hook_callback");
    backend_namespace=dlsym(library,"hybris_create_namespace");
    backend_open_ext=dlsym(library,"hybris_dlopen_ext");
    if(!backend_open || !backend_symbol || !backend_error || !backend_hook || !backend_namespace || !backend_open_ext){
        fprintf(stderr,"[launcher] libhybris backend lacks required APIs\n");return 0;
    }
    return 1;
}

void *k2c_hybris_dlopen(const char *path,int flags){return backend_open(path,flags);}
void *k2c_hybris_dlsym(void *handle,const char *name){return backend_symbol(handle,name);}
char *k2c_hybris_dlerror(void){return backend_error();}
void k2c_hybris_set_hook_callback(void *(*callback)(const char *,const char *)){backend_hook(callback);}
void *k2c_hybris_create_namespace(const char *name,const char *paths,const char *defaults,uint64_t type,const char *permitted,void *parent)
{return backend_namespace(name,paths,defaults,type,permitted,parent);}
void *k2c_hybris_dlopen_ext(const char *path,int flags,const void *ext){return backend_open_ext(path,flags,ext);}
