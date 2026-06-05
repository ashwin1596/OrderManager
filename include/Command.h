#pragma once

#include <cstdint>

#include "Orderbook.h"

struct Command {
    enum Type : std::uint8_t { Add, Cancel, Modify, Shutdown} type;
    OrderId id;
    Price price;
    Quantity qty;
    Side side;
};