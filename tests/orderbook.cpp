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

void test_fills_reported(){
    Orderbook book;
    book.add(1, 100, 10, Side::Ask);

    std::vector<Fill> fills;
    book.add(2, 100, 4, Side::Bid, [&](const Fill& f){ fills.push_back(f); });

    assert(fills.size() == 1);
    assert(fills[0].qty == 4);
    assert(fills[0].price == 100);
    assert(fills[0].resting_Id == 1);
    assert(fills[0].aggressor_id == 2);
    std::cout << "test_fills_reported PASSED\n";
}

void test_modify_order_fast_path_decrease_qty_same_price(){
    Orderbook book;
    book.add(1, 100, 10, Side::Ask);

    std::vector<Fill> fills;
    book.modify(1, 100, 7, [&](const Fill& f){ fills.push_back(f); });

    assert(book.ask_qty(100) == 7);
    assert(book.has_order(1));
    assert(fills.empty());
    std::cout << "test_modify_order_fast_path_decrease_qty_same_price PASSED\n";
}

void test_modify_order_zero_qty_removes_order(){
    Orderbook book;
    book.add(1, 100, 10, Side::Ask);

    std::vector<Fill> fills;
    book.modify(1, 100, 0, [&](const Fill& f){ fills.push_back(f); });

    assert(!book.has_ask(100));
    assert(!book.has_order(1));
    assert(fills.empty());
    std::cout << "test_modify_order_zero_qty_removes_order PASSED\n";
}

void test_modify_order_increase_qty_same_price(){
    Orderbook book;
    book.add(1, 100, 10, Side::Ask);

    std::vector<Fill> fills;
    book.modify(1, 100, 15, [&](const Fill& f){ fills.push_back(f); });

    assert(book.ask_qty(100) == 15);
    assert(book.has_order(1));
    assert(fills.empty());
    std::cout << "test_modify_order_increase_qty_same_price PASSED\n";
}

void test_modify_order_reprice_no_cross(){
    Orderbook book;
    book.add(1, 100, 10, Side::Ask);
    book.add(2, 99, 5, Side::Ask);

    std::vector<Fill> fills;
    book.modify(1, 101, 10, [&](const Fill& f){ fills.push_back(f); });

    assert(!book.has_ask(100));
    assert(book.has_ask(101));
    assert(book.ask_qty(101) == 10);
    assert(book.has_order(1));
    assert(fills.empty());
    std::cout << "test_modify_order_reprice_no_cross PASSED\n";
}

void test_modify_order_reprice_with_cross(){
    Orderbook book;
    book.add(1, 100, 10, Side::Ask);
    book.add(2, 95, 5, Side::Bid);
    book.add(3, 98, 5, Side::Bid);

    std::vector<Fill> fills;
    book.modify(3, 102, 3, [&](const Fill& f){ fills.push_back(f); });

    assert(fills.size() == 1);
    assert(fills[0].qty == 3);
    assert(fills[0].price == 100);
    assert(fills[0].resting_Id == 1);
    assert(fills[0].aggressor_id == 3);
    assert(book.ask_qty(100) == 7);
    std::cout << "test_modify_order_reprice_with_cross PASSED\n";
}

void test_modify_order_qty_increase_loses_priority(){
    Orderbook book;
    book.add(1, 100, 10, Side::Bid);
    book.add(2, 100, 20, Side::Bid);

    std::vector<Fill> fills;
    book.modify(1, 100, 15, [&](const Fill& f){ fills.push_back(f); });
    book.add(3, 100, 30, Side::Ask, [&](const Fill& f){ fills.push_back(f); });

    assert(fills.size() == 2);
    assert(fills[0].qty == 20);
    assert(fills[0].resting_Id == 2);
    assert(fills[0].aggressor_id == 3);
    assert(fills[1].qty == 10);
    assert(fills[1].resting_Id == 1);
    assert(fills[1].aggressor_id == 3);
    std::cout << "test_modify_order_qty_increase_loses_priority PASSED\n";
}

void test_modify_order_unknown_id(){
    Orderbook book;
    book.add(1, 100, 10, Side::Ask);

    std::vector<Fill> fills;
    bool result = book.modify(999, 100, 5, [&](const Fill& f){ fills.push_back(f); });

    assert(!result);
    assert(book.ask_qty(100) == 10);
    assert(fills.empty());
    std::cout << "test_modify_order_unknown_id PASSED\n";
}

int main(){
    test_resting_bid_lands_on_bid_side();
    test_crossing_order_fills();
    test_sweep_two_levels();
    test_cancel_order();
    test_fills_reported();
    test_modify_order_fast_path_decrease_qty_same_price();
    test_modify_order_zero_qty_removes_order();
    test_modify_order_increase_qty_same_price();
    test_modify_order_reprice_no_cross();
    test_modify_order_reprice_with_cross();
    test_modify_order_qty_increase_loses_priority();
    test_modify_order_unknown_id();
    std::cout << "ALL TESTS PASSED\n";

    return 0;
}