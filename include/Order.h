#pragma once

#include <cstdint>

struct Order{
    std::uint64_t id;
    std::int64_t price;
    std::uint32_t quantity;
    Order(std::uint64_t id, std::int64_t price, std::uint32_t quantity)
        : id(id), price(price), quantity(quantity) {}
};