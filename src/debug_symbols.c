#include "debug_symbols.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
bool debug_symbols_add(DebugSymbols *s, uint16_t address, const char *name) {
    if (!s || !name || !*name || strlen(name)>=DEBUG_SYMBOL_NAME) return false;
    for (const char *p=name;*p;++p) if (isspace((unsigned char)*p)) return false;
    for (unsigned i=0;i<s->count;++i) if (!strcmp(s->items[i].name,name)) {
        s->items[i].address=address; return true;
    }
    if (s->count==DEBUG_SYMBOLS) return false;
    DebugSymbol *item=&s->items[s->count++]; item->address=address;
    snprintf(item->name,sizeof(item->name),"%s",name); return true;
}
const char *debug_symbols_name(const DebugSymbols *s, uint16_t address) {
    if (s) for (unsigned i=0;i<s->count;++i) if (s->items[i].address==address) return s->items[i].name;
    return NULL;
}
bool debug_symbols_address(const DebugSymbols *s, const char *name, uint16_t *address) {
    if (!s || !name || !address) return false;
    for (unsigned i=0;i<s->count;++i) if (!strcmp(s->items[i].name,name)) { *address=s->items[i].address; return true; }
    return false;
}
bool debug_symbols_load(DebugSymbols *s, const char *path) {
    if (!s || !path) return false;
    FILE *f=fopen(path,"r"); if(!f) return false;
    DebugSymbols *staging=calloc(1,sizeof(*staging)); if(!staging) { fclose(f); return false; }
    bool ok=true; char line[256];
    while (ok && fgets(line,sizeof(line),f)) {
        if (!strchr(line,'\n') && !feof(f)) { ok=false; break; }
        char *p=line; while(isspace((unsigned char)*p)) ++p;
        if (!*p || *p=='#' || *p==';') continue;
        if (!strncmp(p,"al ",3) || !strncmp(p,"al\t",3)) p+=3;
        if (*p=='$') ++p;
        char *end; unsigned long addr=strtoul(p,&end,16);
        if (p==end || addr>65535 || !isspace((unsigned char)*end)) { ok=false; break; }
        p=end; while(isspace((unsigned char)*p)) ++p;
        if (*p=='.') ++p;
        end=p; while(*end && !isspace((unsigned char)*end)) ++end;
        if (*end) *end++=0;
        while(isspace((unsigned char)*end)) ++end;
        if (*end && *end!='#' && *end!=';') { ok=false; break; }
        ok=debug_symbols_add(staging,(uint16_t)addr,p);
    }
    if(ferror(f)) ok=false;
    if(fclose(f)) ok=false;
    if(ok) *s=*staging;
    free(staging); return ok;
}
