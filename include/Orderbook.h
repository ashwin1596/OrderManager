#pragma once
#include <cstdint>
#include <map>
#include <unordered_map>
#include <list>
#include <functional>   // std::greater
#include <algorithm>
#include <cassert>
#include <optional>

enum class Side : std::uint8_t { Bid, Ask };

// Plain aliases: raw CPU performance, zero compile-time protection against
// mixing prices and quantities. Strong typedefs (a wrapper struct per type)
// would add type-safety at the cost of boilerplate — a reasonable upgrade later.
using Price    = std::uint64_t;   // fixed-point (e.g. ticks / micro-cents)
using Quantity = std::uint32_t;   // up to ~4.2B per order
using OrderId  = std::uint64_t;   // unique system-wide

struct Order {
    OrderId  id;        // set-once by convention (no setter), but NOT const — keeps Order assignable
    Price    price;     // mutable: amend / repricing
    Quantity quantity;  // mutable: decreases on partial fills
    Side     side;

    Order(OrderId id_, Price price_, Quantity quantity_, Side side_)
        : id(id_), price(price_), quantity(quantity_), side(side_) {}
};

struct Level {
    std::list<Order> orders;
    Quantity total_quantity = 0;   // running sum — O(1) "size at this level"
};

struct OrderLocation {
    Side  side;
    Price price;
    std::list<Order>::iterator it;

    OrderLocation(Side side_, Price price_, std::list<Order>::iterator it_)
        : side(side_), price(price_), it(it_) {}
};

struct Fill {
    Price price;
    Quantity qty;
    OrderId resting_Id;
    OrderId aggressor_id;

    Fill (Price price_, Quantity qty_, OrderId resting_id_, OrderId aggressor_id_): price(price_), qty(qty_), resting_Id(resting_id_), aggressor_id(aggressor_id_) {}
};

struct TopOfBook {
    Price price;
    Quantity qty;
};

class Orderbook {
public:
    Orderbook() = default;

    // Non-copyable and non-movable: the book owns cross-referencing state
    // (iterators into per-level lists; later, Level* in OrderLocation) that
    // makes copying meaningless and moving a correctness hazard. It lives in
    // one place for its lifetime. Store via unique_ptr<Orderbook> if you need
    // to relocate ownership.
    Orderbook(const Orderbook&)            = delete;
    Orderbook& operator=(const Orderbook&) = delete;
    Orderbook(Orderbook&&)                 = delete;
    Orderbook& operator=(Orderbook&&)      = delete;

    ~Orderbook() = default;

    void add(OrderId orderId, Price price, Quantity qty, Side side){
        add(orderId, price, qty, side, [](const Fill&){});
    }

    template <typename FillHandler>
    void add(OrderId orderId, Price price, Quantity qty, Side side, FillHandler&& on_fill){
        
        if(order_index_.count(orderId) > 0)
            return;

        Order order(orderId, price, qty, side);
        
        
        // match against the resting orders
        matchOrder(order, on_fill);

        // add the remaining to the opposite side of the book
        if (order.quantity > 0){
            restOrder(order);  // add the remain to the book
        }
    }

    bool cancel(OrderId id) {
        auto ord_it = order_index_.find(id);
        if (ord_it == order_index_.end()) return false;

        const OrderLocation& loc = ord_it->second;
        return loc.side == Side::Ask
            ? cancelFrom(asks_, id, loc.price, loc.it)
            : cancelFrom(bids_, id, loc.price, loc.it);
    }

    template <typename FillHandler>
    bool modify(OrderId id, Price new_price, Quantity new_qty, FillHandler&& on_fill){
        auto ord_it = order_index_.find(id);
        if (ord_it == order_index_.end()) return false;

        const OrderLocation& loc = ord_it->second;
        Side side = loc.side;
        Price curr_price = loc.price;
        auto list_it = loc.it;
        Quantity curr_qty = list_it->quantity;
        
        if (new_qty == 0)  { return cancel(id); }

        bool fast_path = (new_price == curr_price) && (new_qty <= curr_qty);

        if (fast_path){
            Quantity qty_change = curr_qty - new_qty;
            list_it->quantity = new_qty;
            side == Side::Ask
            ? reducePriceQty(asks_, curr_price, qty_change)
            : reducePriceQty(bids_, curr_price, qty_change);

            return true;
        }
        else {
            cancel(id);
            add(id, new_price, new_qty, side, std::forward<FillHandler>(on_fill));
            return true;
        }
    }

    bool has_bid(Price price) const {return bids_.count(price) > 0;}
    
    bool has_ask(Price price) const {return asks_.count(price) > 0;}

    Quantity bid_qty(Price p) const {
        auto it = bids_.find(p);
        return it == bids_.end() ? 0 : it->second.total_quantity;
    }

    Quantity ask_qty(Price p) const {
        auto it = asks_.find(p);
        return it == asks_.end() ? 0 : it->second.total_quantity;
    }

    bool has_order(OrderId id) const {
        return order_index_.count(id) > 0;
    }

    std::optional<Price> best_bid() const {
        if (bids_.empty())
            return std::nullopt;
        
        return bids_.begin()->first;
    }

    std::optional<Price> best_ask() const {
        if (asks_.empty())
            return std::nullopt;
        
        return asks_.begin()->first;
    }

    std::optional<Price> spread() const {
        std::optional<Price> best_bid_price = best_bid();
        std::optional<Price> best_ask_price = best_ask();

        if (best_bid_price.has_value() && best_ask_price.has_value()) {
            assert(best_ask_price.value() > best_bid_price.value() && "Best bid is greater than best ask - book invariant doesn't hold!");
            return best_ask_price.value() - best_bid_price.value();
        }
        
        return std::nullopt;
    }

    std::optional<Quantity> best_bid_qty() const {
        if (bids_.empty())
            return std::nullopt;   
            
        return bids_.begin()->second.total_quantity;
    }

    std::optional<Quantity> best_ask_qty() const {
        if (asks_.empty())
            return std::nullopt;
        
        return asks_.begin()->second.total_quantity;
    }

    std::optional<TopOfBook> best_bid_level() const {
        if (bids_.empty())
            return std::nullopt;       
        
        auto level_it = bids_.begin();
        return TopOfBook{level_it->first, level_it->second.total_quantity};
    }

    std::optional<TopOfBook> best_ask_level() const {
        if (asks_.empty())
            return std::nullopt;       
        
        auto level_it = asks_.begin();
        return TopOfBook{level_it->first, level_it->second.total_quantity};
    }

private:
    std::map<Price, Level, std::greater<Price>> bids_;   // highest bid first
    std::map<Price, Level>                      asks_;   // lowest ask first
    std::unordered_map<OrderId, OrderLocation>  order_index_;

    /// @brief Matches the current order with the existing orders if possible
    /// @param order current order to be mathced against
    /// @return order input is passed as reference - any changes to the order in here are reflected back and a vector of fills happened
    template <typename FillHandler>
    void matchOrder(Order& order, FillHandler& on_fill){
        
        if (order.side == Side::Bid){  
            // match against ask_side of the book - cheapest first
            while (order.quantity > 0 && !asks_.empty()){
                auto level_it = asks_.begin();
                Price best_ask = level_it->first;
                if (order.price < best_ask) break;  // no longer crosses the spread - stop

                matchAtLevel(order, level_it->second, on_fill);

                if (level_it->second.orders.empty()){
                    asks_.erase(level_it);  // level exhausted - remove
                }
            }
        }
        else {  // match against bid_side of the book
            // match against bid_side of the book - highest order.price first
            while (order.quantity > 0 && !bids_.empty()){
                auto level_it = bids_.begin();
                Price best_bid = level_it->first;
                if (order.price > best_bid) break; // no longer crosses the spread - stop

                matchAtLevel(order, level_it->second, on_fill);

                if (level_it->second.orders.empty()){
                    bids_.erase(level_it);  // level exhausted - remove
                }
            }
        }
    }

    template <typename FillHandler>
    void matchAtLevel(Order& order, Level& level, FillHandler& on_fill){
        auto it = level.orders.begin();

        while (order.quantity > 0 && it != level.orders.end())
        {
            /* decrement quantities and remove fully filled resting orders */
            Quantity matching_qty = std::min(order.quantity, it->quantity);
            order.quantity -= matching_qty;
            it->quantity -= matching_qty;
            level.total_quantity -= matching_qty;

            on_fill(Fill{it->price, matching_qty, it->id, order.id});

            if (it->quantity == 0) {
                order_index_.erase(it->id);
                it = level.orders.erase(it);   // erase returns next; do NOT ++it
            } else {
                ++it;                          // partial fill — order survives, advance
            }
        }
    }

    void restOrder(Order& order){
        auto list_it = order.side == Side::Bid
                        ? insertInto(bids_, order)
                        : insertInto(asks_, order);
        
        order_index_.emplace(order.id, OrderLocation{order.side, order.price, list_it});
    }

    template <typename BookSide>
    std::list<Order>::iterator insertInto(BookSide& book, const Order& order){
        Level& level = book[order.price];
        level.orders.emplace_back(order);
        level.total_quantity += order.quantity;              
        return std::prev(level.orders.end());
    }

    template <typename BookSide>
    bool cancelFrom(BookSide& book, OrderId id, Price price, std::list<Order>::iterator it){
        auto level_it = book.find(price);

        assert(level_it != book.end() && "index/book desync");

        Level& level = level_it->second;
        level.total_quantity -= it->quantity;
    
        // erasing the order from the book - extract any order details right now if needed
        level.orders.erase(it);

        if (level.orders.empty()){
            book.erase(price);
        }

        order_index_.erase(id);

        return true;
    }

    template <typename BookSide>
    void reducePriceQty(BookSide& book, Price price, Quantity qty_change){
        auto level_it = book.find(price);

        assert(level_it != book.end() && "index/book desync");

        Level& level = level_it->second;
        level.total_quantity -= qty_change;
    }
};