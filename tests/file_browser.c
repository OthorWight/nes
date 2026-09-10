#include "file_browser.h"
#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#define remove_dir(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(p) mkdir(p,0700)
#define remove_dir(p) rmdir(p)
#endif
static FileBrowser browser;
static char output[BROWSER_PATH];
static void touch(const char *path) { FILE *f=fopen(path,"wb"); assert(f); assert(!fclose(f)); }
static bool key(HostKey code, int mod) {
    HostEvent e={.type=HOST_KEYDOWN,.key.keysym={code,mod}};
    return file_browser_event(&browser,&e,1,output);
}
int main(void) {
    const char *root="build/tests/browser-fixture";
    assert(!make_dir(root)); assert(!make_dir("build/tests/browser-fixture/sub folder"));
    touch("build/tests/browser-fixture/game.NES"); touch("build/tests/browser-fixture/ignore.txt");
    touch("build/tests/browser-fixture/sub folder/other.nes");
    assert(file_browser_open(&browser,root,"Open ROM",".nes",false));
    file_browser_resize(&browser,800,600);
    assert(browser.count==2 && browser.entries[0].directory && !strcmp(browser.entries[0].name,"sub folder"));
    assert(!key(HOST_KEY_RETURN,0)); assert(strstr(browser.path,"sub folder") && browser.count==1);
    assert(key(HOST_KEY_RETURN,0)); assert(!strcmp(output,"build/tests/browser-fixture/sub folder/other.nes"));
    assert(file_browser_open(&browser,root,"Open ROM",".nes",false));
    assert(!key('l',HOST_MOD_CTRL)); assert(browser.editing);
    snprintf(browser.location,sizeof(browser.location),"build/tests/browser-fixture/missing");
    assert(!key(HOST_KEY_RETURN,0)); assert(browser.active && *browser.error && !strcmp(browser.path,root));
    snprintf(browser.location,sizeof(browser.location),"build/tests/browser-fixture/game.NES");
    assert(key(HOST_KEY_RETURN,0)); assert(strstr(output,"game.NES"));
    assert(file_browser_open(&browser,root,"Open ROM",".nes",false));
    assert(!key(HOST_KEY_DOWN,0)); assert(browser.selected==1);
    assert(!key(HOST_KEY_ESCAPE,0)); assert(!browser.active);
    assert(file_browser_open(&browser,root,"Save State",".state",true));
    assert(browser.count==1 && browser.entries[0].directory);
    file_browser_resize(&browser,256,276); assert(browser.rows>0 && browser.bounds.w==240);
    assert(!remove("build/tests/browser-fixture/sub folder/other.nes"));
    assert(!remove_dir("build/tests/browser-fixture/sub folder"));
    assert(!remove("build/tests/browser-fixture/game.NES")); assert(!remove("build/tests/browser-fixture/ignore.txt"));
    assert(!remove_dir(root));
    puts("File browser folder traversal, filters, paths, errors and cancel passed");
}
