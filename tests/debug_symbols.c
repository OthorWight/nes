#include "debug_symbols.h"
#include "state_io.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    DebugSymbols s={0}; uint16_t addr;
    const char *path="build/tests/debug-symbols.lbl";
    const char *input="; exported labels\nal 008000 .Reset\nal 009000 .Main\n$0010 counter # RAM\n";
    assert(state_atomic_write(path,input,strlen(input)));
    assert(debug_symbols_load(&s,path) && s.count==3);
    assert(debug_symbols_address(&s,"Reset",&addr) && addr==0x8000);
    assert(!strcmp(debug_symbols_name(&s,0x10),"counter"));
    assert(debug_symbols_add(&s,0x9001,"Main") && s.count==3);
    assert(debug_symbols_address(&s,"Main",&addr) && addr==0x9001);
    input="al 008000 .Valid\nal 1000000 .Invalid\n";
    assert(state_atomic_write(path,input,strlen(input)));
    assert(!debug_symbols_load(&s,path) && s.count==3);
    assert(debug_symbols_address(&s,"Main",&addr) && addr==0x9001);
    assert(!debug_symbols_add(&s,0,"bad name")); assert(!remove(path));
    puts("Symbol import, lookup, replacement and atomic failure checks passed.");
}
