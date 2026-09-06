#include "mappers.h"

void mapper_004_init(Cartridge *cart) {
    cart->mapper_id = 4;
    mapper_mmc3_init(cart);
}
