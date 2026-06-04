#include "../include/Order.h"
#include "../include/ObjectPool.h"

#include <iostream>

int main(){
    ObjectPool<Order> orderPool(100'000);  // Create an object pool with capacity for 100,000 orders

    // On the hot path, we can allocate and deallocate orders without heap allocation or deallocation.
    Order* o = orderPool.allocate(12345, 100, 10);  // Allocate an order with id=12345, price=100, quantity=10
    if (o == nullptr) {
        std::cerr << "Failed to allocate order from pool." << std::endl;  // Handle pool exhaustion (e.g., log an error, throw an exception, etc.)
    }

    orderPool.deallocate(o);  // Deallocate the order, returning it to the pool for reuse

    return 0;
}