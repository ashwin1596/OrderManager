#include "../include/SPSCQueue.h"
#include "../include/Orderbook.h"
#include "../include/Command.h"

#include <atomic>
#include <vector>
#include <cassert>
#include <iostream>

// The matching stage: pop Commands from inbound, dispatch to the book, push
// resulting Fills to outbound. Single-threaded-safe to call directly for testing;
// in stage 3 this becomes the body of the matching thread.
//
// Exit conditions:
//   - a Shutdown command (the poison pill) is popped, OR
//   - the inbound queue is empty AND the feed has signalled it is done.
template <std::size_t N, std::size_t M>
void matching_loop(SPSCQueue<Command, N>& inbound,
                   SPSCQueue<Fill, M>& outbound,
                   std::atomic<bool>& feed_done) {
    Orderbook book(1'000'000);

    // The fill sink: never drop a fill — spin until the outbound queue has room.
    // (In stage 3 this spin gets an _mm_pause() hint.)
    auto on_fill = [&outbound](const Fill& f) {
        while (!outbound.push(f)) { /* _mm_pause(); */ }
    };

    Command cmd;
    for (;;) {
        if (inbound.pop(cmd)) {
            if (cmd.type == Command::Shutdown) break;     // poison pill — stop
            switch (cmd.type) {
                case Command::Add:
                    book.add(cmd.id, cmd.price, cmd.qty, cmd.side, on_fill);
                    break;
                case Command::Cancel:
                    book.cancel(cmd.id);
                    break;
                case Command::Modify:
                    book.modify(cmd.id, cmd.price, cmd.qty, on_fill);
                    break;
                case Command::Shutdown:
                    break;                                // handled above; for -Wswitch
            }
        } else if (feed_done.load(std::memory_order_acquire)) {
            break;                                        // empty AND feed done — drained
        } else {
            /* _mm_pause(); */                            // empty, not done — spin (stage 3)
        }
    }
}

// Helper: drain all fills currently in the outbound queue into a vector.
template <std::size_t M>
std::vector<Fill> drain(SPSCQueue<Fill, M>& outbound) {
    std::vector<Fill> fills;
    Fill f{0, 0, 0, 0};
    while (outbound.pop(f)) {
        fills.push_back(f);
    }
    return fills;
}

void test_pipeline_single_threaded_basic_fill() {
    SPSCQueue<Command, 4096> inbound;
    SPSCQueue<Fill, 4096>    outbound;
    std::atomic<bool>        feed_done{false};

    // Pre-load the workload, ending with the poison pill.
    inbound.push(Command{Command::Add,      1, 100, 10, Side::Ask});  // resting ask: sell 10 @ 100
    inbound.push(Command{Command::Add,      2, 100,  4, Side::Bid});  // crossing bid: buy 4 @ 100 -> 1 fill
    inbound.push(Command{Command::Shutdown, 0,   0,  0, Side::Bid});  // stop

    matching_loop(inbound, outbound, feed_done);

    std::vector<Fill> fills = drain(outbound);

    assert(fills.size() == 1);
    assert(fills[0].qty == 4);
    assert(fills[0].price == 100);          // resting order's price
    assert(fills[0].resting_Id == 1);
    assert(fills[0].aggressor_id == 2);
    std::cout << "test_pipeline_single_threaded_basic_fill PASSED\n";
}

void test_pipeline_single_threaded_sweep_and_cancel() {
    SPSCQueue<Command, 4096> inbound;
    SPSCQueue<Fill, 4096>    outbound;
    std::atomic<bool>        feed_done{false};

    // Two resting asks at different levels, one cancel, then a bid that sweeps.
    inbound.push(Command{Command::Add,    1, 100, 5, Side::Ask});  // 5 @ 100
    inbound.push(Command{Command::Add,    2, 101, 5, Side::Ask});  // 5 @ 101
    inbound.push(Command{Command::Add,    3, 100, 2, Side::Ask});  // 2 @ 100 (behind id 1)
    inbound.push(Command{Command::Cancel, 1,   0, 0, Side::Ask});  // cancel id 1 -> 100 level now just id 3 (qty 2)
    inbound.push(Command{Command::Add,    4, 101, 6, Side::Bid});  // buy 6 up-to-101: takes 2@100 (id3), 4@101 (id2)
    inbound.push(Command{Command::Shutdown, 0, 0, 0, Side::Bid});

    matching_loop(inbound, outbound, feed_done);

    std::vector<Fill> fills = drain(outbound);

    // Expect two fills: id3 (2 @ 100) first, then id2 (4 @ 101).
    assert(fills.size() == 2);
    assert(fills[0].resting_Id == 3);
    assert(fills[0].qty == 2);
    assert(fills[0].price == 100);
    assert(fills[0].aggressor_id == 4);
    assert(fills[1].resting_Id == 2);
    assert(fills[1].qty == 4);
    assert(fills[1].price == 101);
    assert(fills[1].aggressor_id == 4);
    std::cout << "test_pipeline_single_threaded_sweep_and_cancel PASSED\n";
}

void test_pipeline_single_threaded_drain_on_feed_done() {
    // Exercise the OTHER exit path: no poison pill, but feed_done is set and the
    // queue empties. The loop should process everything then exit.
    SPSCQueue<Command, 4096> inbound;
    SPSCQueue<Fill, 4096>    outbound;
    std::atomic<bool>        feed_done{true};   // feed already finished

    inbound.push(Command{Command::Add, 1, 100, 10, Side::Ask});
    inbound.push(Command{Command::Add, 2, 100,  3, Side::Bid});  // 1 fill, qty 3
    // no Shutdown pill — relies on (empty AND feed_done) to exit

    matching_loop(inbound, outbound, feed_done);

    std::vector<Fill> fills = drain(outbound);
    assert(fills.size() == 1);
    assert(fills[0].qty == 3);
    assert(fills[0].resting_Id == 1);
    std::cout << "test_pipeline_single_threaded_drain_on_feed_done PASSED\n";
}

int main() {
    test_pipeline_single_threaded_basic_fill();
    test_pipeline_single_threaded_sweep_and_cancel();
    test_pipeline_single_threaded_drain_on_feed_done();
    std::cout << "ALL PIPELINE STAGE-2 TESTS PASSED\n";
    return 0;
}