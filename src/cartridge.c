#include "cartridge.h"
#include "mappers.h"
#include "nes_system.h"
#include "state_io.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool generate_save_filepath(char *dest, const char *src, size_t max_len) {
    const char *ext = strrchr(src, '.');
    const char *slash = strrchr(src, '/');
    const char *backslash = strrchr(src, '\\');
    size_t stem = strlen(src);
    if (ext && (!slash || ext > slash) && (!backslash || ext > backslash)) stem = (size_t)(ext - src);
    if (stem + 5 > max_len) return false;
    memcpy(dest, src, stem);
    memcpy(dest + stem, ".sav", 5);
    return true;
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
        case 5:   mapper_005_init(cart); break;
        case 7:   mapper_007_init(cart); break;
        case 9:   mapper_009_init(cart); break;
        case 10:  mapper_010_init(cart); break;
        case 11:  mapper_011_init(cart); break;
        case 19:  mapper_019_init(cart); break;
        case 23:  mapper_023_init(cart); break;
        case 24:  mapper_024_init(cart); break;
        case 26:  mapper_026_init(cart); break;
        case 34:  mapper_034_init(cart); break;
        case 64:  mapper_064_init(cart); break;
        case 66:  mapper_066_init(cart); break;
        case 69:  mapper_069_init(cart); break;
        case 71:  mapper_071_init(cart); break;
        case 78:  mapper_078_init(cart); break;
        case 118: mapper_118_init(cart); break;
        case 206: mapper_206_init(cart); break;
        case 227: mapper_227_init(cart); break;
        default:
            fprintf(stderr, "Error: Unsupported mapper ID %u\n", cart->mapper_id);
            return false;
    }
    return cart->vtable && (!cart->vtable->state_size || cart->mapper_data);
}

static bool load_error(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return false;
}

static bool supported_board(const CartridgeInfo *i, char *error, size_t size) {
    switch (i->mapper_id) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 7: case 9:
        case 10: case 11: case 19: case 23: case 24: case 26: case 34:
        case 64: case 66: case 69: case 71: case 78: case 118: case 206: case 227: break;
        default:
            if (error && size) snprintf(error, size, "Unsupported mapper %u", i->mapper_id);
            return false;
    }
    if (i->nes2) {
        // NES 2.0 sizes must fit the implemented address lines. Larger boards
        // need an outer-bank/submapper implementation, not silent wrapping.
        uint32_t prg_limit = 64u * 1024 * 1024, chr_limit = 8u * 1024 * 1024;
        switch (i->mapper_id) {
            case 0: prg_limit = 32768; chr_limit = 8192; break;
            case 1: prg_limit = 262144; chr_limit = 131072; break;
            case 4: prg_limit = 524288; chr_limit = 262144; break;
            case 5: prg_limit = 1048576; chr_limit = 1048576; break;
            case 9: prg_limit = 131072; chr_limit = 131072; break;
            case 10: prg_limit = 262144; chr_limit = 131072; break;
            case 78: prg_limit = 131072; chr_limit = 131072; break;
            case 118: prg_limit = 524288; chr_limit = 131072; break;
            default: break;
        }
        if (i->prg_rom_size > prg_limit || i->chr_rom_size > chr_limit)
            return load_error(error, size, "Extended ROM banking unsupported");
    }
    // Board-specific NES 2.0 variants (MMC6, bus conflicts, etc.) require
    // separate implementations; never silently run them as submapper zero.
    if (i->submapper && !(i->mapper_id == 78 && (i->submapper == 1 || i->submapper == 3)))
        return load_error(error, size, "Submapper unsupported");
    if (i->timing == 1 || i->timing == 3) return load_error(error, size, "PAL/Dendy timing unsupported");
    if (i->prg_rom_size < 16384) return load_error(error, size, "PRG layout unsupported");
    if (i->prg_ram_size && i->prg_nvram_size) return load_error(error, size, "Mixed PRG RAM unsupported");
    if (i->chr_ram_size && i->chr_nvram_size) return load_error(error, size, "Mixed CHR RAM unsupported");
    uint32_t prg_ram = i->prg_ram_size + i->prg_nvram_size;
    uint32_t chr_ram = i->chr_ram_size + i->chr_nvram_size;
    if (prg_ram > (i->mapper_id == 5 ? 65536u : 8192u)) return load_error(error, size, "PRG RAM banking unsupported");
    if (i->mapper_id == 5 && prg_ram && prg_ram < 8192) return load_error(error, size, "MMC5 RAM layout unsupported");
    if (i->chr_rom_size && chr_ram) return load_error(error, size, "Mixed CHR ROM/RAM unsupported");
    if (!i->chr_rom_size && chr_ram < 8192) return load_error(error, size, "CHR RAM layout unsupported");
    if (i->battery && !i->prg_nvram_size && !i->chr_nvram_size)
        return load_error(error, size, "Battery storage unsupported");
    if (i->trainer && prg_ram < 8192) return load_error(error, size, "Trainer needs 8 KiB PRG RAM");
    if (i->nes2 && chr_ram > 8192 && (i->mapper_id == 0 || i->mapper_id == 2 ||
        i->mapper_id == 7 || i->mapper_id == 71 || i->mapper_id == 227))
        return load_error(error, size, "CHR RAM banking unsupported");
    // Mapper 78 historically uses this flag to select H/V mirroring wiring,
    // not extra nametable RAM. Its explicit submappers override that hint.
    if (i->mirroring == MIRROR_FOUR_SCREEN && i->mapper_id != 0 && i->mapper_id != 4 &&
        i->mapper_id != 78 && i->mapper_id != 206)
        return load_error(error, size, "Four-screen board unsupported");
    return true;
}

static uint32_t backing_size(uint32_t declared, uint32_t minimum) {
    uint32_t size = minimum;
    while (size < declared) size <<= 1;
    return size;
}
static uint8_t *read_rom(FILE *f, uint32_t declared, uint32_t size) {
    uint8_t *data = malloc(size);
    if (!data) return NULL;
    if (fread(data, 1, declared, f) != declared) { free(data); return NULL; }
    // Mirror the final populated address-line segments into a power-of-two
    // backing. Keep the exact declared byte count separately in CartridgeInfo.
    uint32_t filled = declared;
    while (filled < size) {
        uint32_t chunk = 1;
        while (chunk * 2 <= size - filled && chunk * 2 <= filled) chunk *= 2;
        memcpy(data + filled, data + filled - chunk, chunk);
        filled += chunk;
    }
    return data;
}

Cartridge *cartridge_load_ex(NES *nes, const char *filepath, char *error, size_t error_size) {
    if (error && error_size) *error = '\0';
    if (!nes || !filepath) { load_error(error, error_size, "Invalid cartridge request"); return NULL; }
    FILE *f = fopen(filepath, "rb");
    if (!f) { load_error(error, error_size, "Cannot open ROM"); return NULL; }
    uint8_t header[16];
    CartridgeInfo info;
    if (fread(header, 1, 16, f) != 16) {
        load_error(error, error_size, "Truncated NES header"); fclose(f); return NULL;
    }
    if (!cartridge_parse_header(header, &info, error, error_size) || !supported_board(&info, error, error_size)) {
        fclose(f); return NULL;
    }
    uint64_t required = 16u + (info.trainer ? 512u : 0u) + (uint64_t)info.prg_rom_size + info.chr_rom_size;
    if (fseek(f, 0, SEEK_END) != 0) { load_error(error, error_size, "Cannot inspect ROM size"); fclose(f); return NULL; }
    long length = ftell(f);
    if (length < 0 || (uint64_t)length < required || fseek(f, 16, SEEK_SET) != 0) {
        load_error(error, error_size, "Truncated ROM or trainer"); fclose(f); return NULL;
    }
    Cartridge *cart = calloc(1, sizeof(*cart));
    if (!cart) { load_error(error, error_size, "Cartridge allocation failed"); fclose(f); return NULL; }
    cart->nes = nes;
    cart->info = info;
    cart->mapper_id = info.mapper_id;
    cart->mirroring = info.mirroring;
    cart->chr_is_ram = info.chr_rom_size == 0;
    cart->has_battery = info.battery;
    cart->prg_rom_size = backing_size(info.prg_rom_size, 16384);
    cart->chr_rom_size = cart->chr_is_ram ? info.chr_ram_size + info.chr_nvram_size : backing_size(info.chr_rom_size, 8192);
    cart->prg_ram_size = info.prg_ram_size + info.prg_nvram_size;
    bool ok = !info.trainer || fread(cart->trainer_data, 1, 512, f) == 512;
    if (ok) cart->prg_rom = read_rom(f, info.prg_rom_size, cart->prg_rom_size);
    if (cart->prg_rom) cart->chr_rom = cart->chr_is_ram ? calloc(1, cart->chr_rom_size) : read_rom(f, info.chr_rom_size, cart->chr_rom_size);
    if (cart->prg_ram_size) cart->prg_ram = calloc(1, cart->prg_ram_size);
    if (fclose(f) != 0) ok = false;
    if (!ok || !cart->prg_rom || !cart->chr_rom || (cart->prg_ram_size && !cart->prg_ram)) {
        load_error(error, error_size, "ROM read/allocation failed"); cartridge_free(cart); return NULL;
    }
    if (!generate_save_filepath(cart->save_filepath, filepath, sizeof(cart->save_filepath))) {
        load_error(error, error_size, "Battery save path too long"); cartridge_free(cart); return NULL;
    }
    uint8_t identity_header[528];
    memcpy(identity_header, header, 16);
    if (info.trainer) memcpy(identity_header + 16, cart->trainer_data, 512);
    cart->rom_identity[0] = state_crc32(identity_header, info.trainer ? 528 : 16);
    cart->rom_identity[1] = state_crc32(cart->prg_rom, cart->prg_rom_size);
    cart->rom_identity[2] = cart->chr_is_ram ? 0 : state_crc32(cart->chr_rom, cart->chr_rom_size);
    // Initialize only after every fallible file/ROM operation. Each mapper
    // constructor checks its private allocation before changing hardware lines.
    if (!initialize_mapper(cart)) {
        load_error(error, error_size, "Mapper allocation failed"); cartridge_free(cart); return NULL;
    }
    if (!cartridge_load_battery(cart)) {
        fprintf(stderr, "Cannot load battery save '%s'; preserving the existing file.\n", cart->save_filepath);
    }
    // Trainers are startup data at $7000-$71FF, after battery RAM restoration.
    if (info.trainer) memcpy(cart->prg_ram + 0x1000, cart->trainer_data, 512);
    return cart;
}

Cartridge *cartridge_load(NES *nes, const char *filepath) {
    char error[128];
    Cartridge *cart = cartridge_load_ex(nes, filepath, error, sizeof(error));
    if (!cart) fprintf(stderr, "%s: %s\n", filepath ? filepath : "(null)", error);
    return cart;
}

uint8_t cartridge_open_bus(const Cartridge *cart) { return cart && cart->nes ? cart->nes->cpu_open_bus : 0; }
uint8_t cartridge_ram_read(const Cartridge *cart, uint32_t offset) {
    return cart && cart->prg_ram && cart->prg_ram_size ? cart->prg_ram[offset % cart->prg_ram_size] : cartridge_open_bus(cart);
}
void cartridge_ram_write(Cartridge *cart, uint32_t offset, uint8_t value) {
    if (cart && cart->prg_ram && cart->prg_ram_size) cart->prg_ram[offset % cart->prg_ram_size] = value;
}
void cartridge_chr_write(Cartridge *cart, uint32_t offset, uint8_t value) {
    if (cart && cart->chr_is_ram && cart->chr_rom && cart->chr_rom_size) cart->chr_rom[offset % cart->chr_rom_size] = value;
}


bool cartridge_load_battery(Cartridge *cart) {
    if (!cart || !cart->has_battery) return true;
    uint32_t prg_size = cart->info.prg_nvram_size;
    uint32_t chr_size = cart->info.chr_nvram_size;
    size_t size = (size_t)prg_size + chr_size;
    if (!size) return true;
    FILE *f = fopen(cart->save_filepath, "rb");
    if (!f) {
        if (errno == ENOENT) return true;
        cart->battery_save_blocked = true;
        return false;
    }
    uint8_t *ram = malloc(size);
    bool ok = ram && fread(ram, 1, size, f) == size &&
              fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (ok) {
        if (prg_size) memcpy(cart->prg_ram, ram, prg_size);
        if (chr_size) memcpy(cart->chr_rom, ram + prg_size, chr_size);
    }
    free(ram);
    cart->battery_save_blocked = !ok;
    return ok;
}

bool cartridge_set_save_path(Cartridge *cart, const char *path) {
    if (!cart || !path || !*path || strlen(path) >= sizeof(cart->save_filepath)) return false;
    strcpy(cart->save_filepath, path);
    // A canonical save takes precedence. If it does not exist, retain the
    // legacy ROM-adjacent RAM loaded earlier; never delete that legacy file.
    bool ok = cartridge_load_battery(cart) && !cart->battery_save_blocked;
    if (ok && cart->info.trainer) memcpy(cart->prg_ram + 0x1000, cart->trainer_data, 512);
    return ok;
}

bool cartridge_save_battery(Cartridge *cart) {
    if (!cart || !cart->has_battery) return true;
    uint32_t prg_size = cart->info.prg_nvram_size;
    uint32_t chr_size = cart->info.chr_nvram_size;
    size_t size = (size_t)prg_size + chr_size;
    if (!size) return true;
    if (cart->battery_save_blocked || (prg_size && !cart->prg_ram) || (chr_size && !cart->chr_rom)) return false;
    uint8_t *ram = malloc(size);
    if (!ram) return false;
    if (prg_size) memcpy(ram, cart->prg_ram, prg_size);
    if (chr_size) memcpy(ram + prg_size, cart->chr_rom, chr_size);
    bool ok = state_atomic_write(cart->save_filepath, ram, size);
    free(ram);
    return ok;
}

void cartridge_free(Cartridge *cart) {
    if (cart) {
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
