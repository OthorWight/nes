#include "md5.h"
#include <string.h>

static void block(uint32_t h[4], const uint8_t data[64]) {
    static const uint32_t k[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const unsigned shifts[16] = {7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
    uint32_t w[16], a=h[0], b=h[1], c=h[2], d=h[3];
    for (unsigned i=0;i<16;++i) w[i]=(uint32_t)data[i*4] | (uint32_t)data[i*4+1]<<8 |
        (uint32_t)data[i*4+2]<<16 | (uint32_t)data[i*4+3]<<24;
    for (unsigned i=0;i<64;++i) {
        unsigned phase=i/16, g;
        uint32_t f;
        if (phase==0) { f=(b&c)|(~b&d); g=i; }
        else if (phase==1) { f=(d&b)|(~d&c); g=(5*i+1)%16; }
        else if (phase==2) { f=b^c^d; g=(3*i+5)%16; }
        else { f=c^(b|~d); g=(7*i)%16; }
        uint32_t t=a+f+k[i]+w[g]; unsigned s=shifts[phase*4+i%4];
        a=d; d=c; c=b; b+=(t<<s)|(t>>(32-s));
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
}

void nes_md5(const void *first, size_t first_size, const void *second, size_t second_size, uint8_t digest[16]) {
    uint32_t h[4]={0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    uint8_t pending[64]; size_t used=0;
    const uint8_t *parts[2]={first,second}; size_t sizes[2]={first_size,second_size};
    for (unsigned p=0;p<2;++p) {
        const uint8_t *data=parts[p]; size_t left=sizes[p];
        while (left) {
            size_t n=64-used; if (n>left) n=left;
            memcpy(pending+used,data,n); used+=n; data+=n; left-=n;
            if (used==64) { block(h,pending); used=0; }
        }
    }
    uint64_t bits=((uint64_t)first_size+second_size)*8;
    pending[used++]=0x80;
    if (used>56) { memset(pending+used,0,64-used); block(h,pending); used=0; }
    memset(pending+used,0,56-used);
    for (unsigned i=0;i<8;++i) pending[56+i]=(uint8_t)(bits>>(i*8));
    block(h,pending);
    for (unsigned i=0;i<16;++i) digest[i]=(uint8_t)(h[i/4]>>(8*(i%4)));
}
