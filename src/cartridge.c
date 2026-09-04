#include "cartridge.h"
#include "mappers.h"
#include "nes_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INES_SIGNATURE_N      'N'
#define INES_SIGNATURE_E      'E'
#define INES_SIGNATURE_S      'S'
#define INES_SIGNATURE_EOF    0x1A

#define PRG_CHUNK_SIZE        16384
#define CHR_CHUNK_SIZE        8192
#define TRAINER_SIZE          512

#define PRG_RAM_DEFAULT_SIZE  8192
#define MMC5_PRG_RAM_SIZE     65536

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

static inline bool validate_ines_header(const uint8_t *header) {
    return (header[0] == INES_SIGNATURE_N &&
            header[1] == INES_SIGNATURE_E &&
            header[2] == INES_SIGNATURE_S &&
            header[3] == INES_SIGNATURE_EOF);
}

static inline bool ines_has_trainer(uint8_t flags6) {
    return (flags6 & 0x04) != 0;
}

static inline bool ines_has_battery(uint8_t flags6) {
    return (flags6 & 0x02) != 0;
}

static inline uint8_t ines_get_mapper_id(uint8_t flags6, uint8_t flags7) {
    return (uint8_t)((flags7 & 0xF0) | (flags6 >> 4));
}

static inline MirroringMode ines_get_mirroring(uint8_t flags6) {
    if (flags6 & 0x08) {
        return MIRROR_FOUR_SCREEN;
    }
    return (flags6 & 0x01) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL;
}

static void generate_save_filepath(char *dest, const char *src, size_t max_len) {
    strncpy(dest, src, max_len - 5);
    dest[max_len - 5] = '\0';
    char *ext = strrchr(dest, '.');
    if (ext) {
        strcpy(ext, ".sav");
    } else {
        strcat(dest, ".sav");
    }
}

uint16_t cartridge_default_remap_ciram(MirroringMode mode, uint16_t addr) {
    uint16_t offset = addr & 0x0FFF;
    switch (mode) {
        case MIRROR_HORIZONTAL:
            return (offset & 0x0800) ? 0x0400 + (offset & 0x03FF) : (offset & 0x03FF);
        case MIRROR_VERTICAL:
            return (offset & 0x0400) ? 0x0400 + (offset & 0x03FF) : (offset & 0x03FF);
        case MIRROR_ONE_SCREEN_LOW:
            return (offset & 0x03FF);
        case MIRROR_ONE_SCREEN_HIGH:
            return 0x0400 + (offset & 0x03FF);
        case MIRROR_FOUR_SCREEN:
        default:
            return offset & 0x0FFF;
    }
}

static bool initialize_mapper(Cartridge *cart) {
    switch (cart->mapper_id) {
        case 0:   mapper_000_init(cart); break;
        case 1:   mapper_001_init(cart); break;
        case 2:   mapper_002_init(cart); break;
        case 3:   mapper_003_init(cart); break;
        case 4:   mapper_004_init(cart); break;
        case 5:
            cart->prg_ram_size = MMC5_PRG_RAM_SIZE;
            free(cart->prg_ram);
            cart->prg_ram = calloc(1, cart->prg_ram_size);
            mapper_005_init(cart);
            break;
        case 7:   mapper_007_init(cart); break;
        case 9:   mapper_009_init(cart); break;
        case 10:  mapper_010_init(cart); break;
        case 11:  mapper_011_init(cart); break;
        case 19:  mapper_019_init(cart); break;
        case 23:  mapper_023_init(cart); break;
        case 34:  mapper_034_init(cart); break;
        case 66:  mapper_066_init(cart); break;
        case 69:  mapper_069_init(cart); break;
        case 71:  mapper_071_init(cart); break; // Added Mapper 71
        case 206: mapper_206_init(cart); break;
        case 227: mapper_227_init(cart); break;
        default:
            fprintf(stderr, "Error: Unsupported mapper ID %u\n", cart->mapper_id);
            return false;
    }
    return true;
}

Cartridge* cartridge_load(NES *nes, const char *filepath) {
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

    if (!validate_ines_header(header)) {
        fprintf(stderr, "Error: Invalid iNES header signature in '%s'\n", filepath);
        fclose(f);
        return NULL;
    }

    Cartridge *cart = calloc(1, sizeof(Cartridge));
    if (!cart) {
        fclose(f);
        return NULL;
    }

    cart->nes = nes;

    uint8_t prg_rom_chunks = header[4];
    uint8_t chr_rom_chunks = header[5];
    uint8_t flags6 = header[6];
    uint8_t flags7 = header[7];

    cart->prg_rom_size = prg_rom_chunks * PRG_CHUNK_SIZE;
    cart->chr_rom_size = chr_rom_chunks * CHR_CHUNK_SIZE;
    cart->mapper_id = ines_get_mapper_id(flags6, flags7);
    cart->mirroring = ines_get_mirroring(flags6);

    if (ines_has_trainer(flags6)) {
        fseek(f, TRAINER_SIZE, SEEK_CUR);
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
        cart->chr_rom_size = CHR_CHUNK_SIZE;
        cart->chr_rom = calloc(1, CHR_CHUNK_SIZE);
    }

    fclose(f);

    cart->prg_ram_size = PRG_RAM_DEFAULT_SIZE;
    cart->prg_ram = calloc(1, cart->prg_ram_size);

    generate_save_filepath(cart->save_filepath, filepath, sizeof(cart->save_filepath));
    cart->has_battery = ines_has_battery(flags6);
    if (cart->has_battery) {
        FILE *sf = fopen(cart->save_filepath, "rb");
        if (sf) {
            fread(cart->prg_ram, 1, cart->prg_ram_size, sf);
            fclose(sf);
            printf("Loaded battery-backed save file: %s\n", cart->save_filepath);
        }
    }

    if (!initialize_mapper(cart)) {
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
        if (cart->vtable && cart->vtable->destroy) {
            cart->vtable->destroy(cart);
        } else if (cart->mapper_data) {
            free(cart->mapper_data);
        }
        free(cart->prg_rom);
        free(cart->chr_rom);
        free(cart->prg_ram);
        free(cart);
    }
}