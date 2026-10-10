#ifndef NES_MD5_H
#define NES_MD5_H
#include <stddef.h>
#include <stdint.h>
/* FM2 identifies the concatenated PRG/CHR payload with MD5. Not used for security. */
void nes_md5(const void *first, size_t first_size, const void *second, size_t second_size,
             uint8_t digest[16]);
#endif
