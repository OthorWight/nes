#include "mappers.h"

void mapper_118_init(Cartridge *cart) {
    cart->mapper_id = 118;
    mapper_mmc3_init(cart);
}
