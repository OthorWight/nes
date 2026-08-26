#include "cartridge.h"
#include "mappers.h"
#include "cpu6502.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==========================================
// POWER-OF-TWO PADDING & MIRRORING HELPER
// ==========================================

static void pad_and_mirror_rom(uint8_t **rom_data, uint32_t *rom_size) {
    uint32_t orig_size = *rom_size;
    if (orig_size == 0) return;
    uint32_t padded_size = 1;
    while (padded_size < orig_size) {
        padded_size <<= 1;
    }
    if (padded_size == orig_size) return;

    uint8_t *new_data = realloc(*rom_data, padded_size);
    if (!new_data) return;
    *rom_data = new_data;

    uint32_t current_size = orig_size;
    while (current_size < padded_size) {
        uint32_t remaining = padded_size - current_size;
        uint32_t chunk_size = 1;
        while (chunk_size * 2 <= remaining && chunk_size * 2 <= current_size) {
            chunk_size *= 2;
        }
        if (chunk_size > remaining) chunk_size = remaining;
        
        memcpy(new_data + current_size, new_data + current_size - chunk_size, chunk_size);
        current_size += chunk_size;
    }
    *rom_size = padded_size;
}

// ==========================================
// LOADER & LIFECYCLE
// ==========================================

Cartridge* cartridge_load(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "Error: Could not open iNES file '%s'\n", filepath);
        return NULL;
    }

    uint8_t header[16];
    if (fread(header, 1, 16, f) != 16) {
        fprintf(stderr, "Error: Failed to read iNES header from '%s'\n", filepath);
        fclose(f);
        return NULL;
    }

    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
        fprintf(stderr, "Error: Invalid iNES header signature in '%s'\n", filepath);
        fclose(f);
        return NULL;
    }

    Cartridge *cart = calloc(1, sizeof(Cartridge));
    if (!cart) {
        fclose(f);
        return NULL;
    }

    uint8_t prg_rom_chunks = header[4];
    uint8_t chr_rom_chunks = header[5];
    uint8_t flags6 = header[6];
    uint8_t flags7 = header[7];

    cart->prg_rom_size = prg_rom_chunks * 16384;
    cart->chr_rom_size = chr_rom_chunks * 8192;
    cart->mapper_id = (uint8_t)((flags7 & 0xF0) | (flags6 >> 4));

    if (flags6 & 0x08) {
        cart->mirroring = MIRROR_FOUR_SCREEN;
    } else if (flags6 & 0x01) {
        cart->mirroring = MIRROR_VERTICAL;
    } else {
        cart->mirroring = MIRROR_HORIZONTAL;
    }

    if (flags6 & 0x04) {
        fseek(f, 512, SEEK_CUR);
    }

    cart->prg_rom = malloc(cart->prg_rom_size);
    if (cart->prg_rom_size > 0 && fread(cart->prg_rom, 1, cart->prg_rom_size, f) != cart->prg_rom_size) {
        fprintf(stderr, "Error: Failed to read PRG-ROM data\n");
        cartridge_free(cart);
        fclose(f);
        return NULL;
    }
    pad_and_mirror_rom(&cart->prg_rom, &cart->prg_rom_size);

    if (cart->chr_rom_size > 0) {
        cart->chr_rom = malloc(cart->chr_rom_size);
        if (fread(cart->chr_rom, 1, cart->chr_rom_size, f) != cart->chr_rom_size) {
            fprintf(stderr, "Error: Failed to read CHR-ROM data\n");
            cartridge_free(cart);
            fclose(f);
            return NULL;
        }
        pad_and_mirror_rom(&cart->chr_rom, &cart->chr_rom_size);
    } else {
        cart->chr_rom_size = 8192;
        cart->chr_rom = calloc(1, 8192);
    }

    fclose(f);

    cart->prg_ram_size = 8192;
    cart->prg_ram = calloc(1, cart->prg_ram_size);

    bool battery = (flags6 & 0x02) != 0;
    cart->has_battery = battery;
    if (battery) {
        strncpy(cart->save_filepath, filepath, sizeof(cart->save_filepath) - 5);
        cart->save_filepath[sizeof(cart->save_filepath) - 5] = '\0';
        char *ext = strrchr(cart->save_filepath, '.');
        if (ext) {
            strcpy(ext, ".sav");
        } else {
            strcat(cart->save_filepath, ".sav");
        }
        FILE *sf = fopen(cart->save_filepath, "rb");
        if (sf) {
            fread(cart->prg_ram, 1, cart->prg_ram_size, sf);
            fclose(sf);
            printf("Loaded battery-backed save file: %s\n", cart->save_filepath);
        }
    }

    if (cart->mapper_id == 0) {
        mapper_000_init(cart);
    } else if (cart->mapper_id == 3) {
        mapper_003_init(cart);
    } else if (cart->mapper_id == 7) {
        mapper_007_init(cart); // Declared / implemented inside mapper_007.c
    } else if (cart->mapper_id == 1) {
        mapper_001_init(cart);
    } else if (cart->mapper_id == 2) {
        mapper_002_init(cart);
    } else if (cart->mapper_id == 4) {
        mapper_004_init(cart);
    } else if (cart->mapper_id == 5) {
        cart->prg_ram_size = 65536;
        free(cart->prg_ram);
        cart->prg_ram = calloc(1, cart->prg_ram_size);
        mapper_005_init(cart);
    } else if (cart->mapper_id == 9) {
        mapper_009_init(cart);
    } else if (cart->mapper_id == 10) {
        mapper_010_init(cart);
    } else if (cart->mapper_id == 69) {
        mapper_069_init(cart);
    } else if (cart->mapper_id == 227) {
        mapper_227_init(cart);
    } else {
        fprintf(stderr, "Error: Unsupported mapper ID %u\n", cart->mapper_id);
        cartridge_free(cart);
        return NULL;
    }

    return cart;
}

void cartridge_save_battery(Cartridge *cart) {
    if (cart && cart->has_battery && cart->prg_ram) {
        FILE *sf = fopen(cart->save_filepath, "wb");
        if (sf) {
            fwrite(cart->prg_ram, 1, cart->prg_ram_size, sf);
            fclose(sf);
            printf("Saved battery-backed progress to: %s\n", cart->save_filepath);
        }
    }
}

void cartridge_free(Cartridge *cart) {
    if (cart) {
        cartridge_save_battery(cart);
        free(cart->prg_rom);
        free(cart->chr_rom);
        free(cart->prg_ram);
        free(cart);
    }
}