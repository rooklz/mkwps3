#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static const Descriptor descriptor{
        Kind::BaseGame,
        "WiiCompiled",
    };
    return descriptor;
}

} // namespace RuntimeProduct
