#include "../include/Orderbook.h"

#include <cassert>
#include <iostream>

void test_resting_bid_lands_on_bid_side(){
    Orderbook book;
    book.add(1, 100, 10, Side::Bid);

    assert(book.has_bid(100));
    assert(!book.has_ask(100));
    assert(book.bid_qty(100) == 10);
    assert(book.has_order(1));
    std::cout << "test_resting_bid_lands_on_bid_side PASSED\n";
}

void test_crossing_order_fills(){
    Orderbook book;
    book.add(1, 100, 10, Side::Ask);
    book.add(2, 100, 4, Side::Bid);

    assert(book.ask_qty(100) == 6);
    assert(!book.has_bid(100));
    assert(book.has_order(1));
    assert(!book.has_order(2));
    std::cout << "test_crossing_order_fills PASSED\n";
}

void test_sweep_two_levels(){
    Orderbook book;
    book.add(1, 100, 5, Side::Ask);
    book.add(2, 101, 5, Side::Ask);
    book.add(3, 101, 8, Side::Bid);

    assert(!book.has_ask(100));
    assert(book.ask_qty(101) == 2);
    assert(!book.has_bid(101));
    assert(!book.has_order(1));
    assert(book.has_order(2));
    std::cout << "test_sweep_two_levels PASSED\n";
}

void test_cancel_order(){
    Orderbook book;
    book.add(1, 101, 5, Side::Ask);
    book.add(2, 101, 12, Side::Ask);

    assert(book.ask_qty(101) == 17);  // total qty for ask price 101 = 12+5 = 17 
    book.cancel(1);  // cancel the order with id 1 - qty reduces to 12
    assert(book.ask_qty(101) == 12);  // remaining ask 101 qty should be 12
    book.cancel(2);  // cancel the order with id 2 - the level would be empty after this so price should not exist anymore in the book
    assert(!book.has_ask(101));

    assert(!book.cancel(2));  // should return false if the order does not exist
    std::cout << "test_cancel_order PASSED\n";
}

int main(){
    test_resting_bid_lands_on_bid_side();
    test_crossing_order_fills();
    test_sweep_two_levels();
    test_cancel_order();
    std::cout << "ALL TESTS PASSED\n";

    return 0;
}