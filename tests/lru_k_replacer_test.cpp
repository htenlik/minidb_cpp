#include "minidb/lru_k_replacer.hpp"
#include "test_utils.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using minidb::LRUKReplacer;
using minidb::test::require;

void accessAndRelease(LRUKReplacer& replacer, minidb::FrameId frameId) {
    replacer.recordAccess(frameId);
    replacer.setEvictable(frameId, true);
}

void testConstructionAndEmpty() {
    minidb::test::requireThrows<std::invalid_argument>(
        [] { LRUKReplacer replacer(0, 2); }, "zero frame capacity was accepted");
    minidb::test::requireThrows<std::invalid_argument>(
        [] { LRUKReplacer replacer(1, 0); }, "K=0 was accepted");
    LRUKReplacer replacer(1, 2);
    require(!replacer.evict().has_value() && replacer.size() == 0,
            "empty replacer returned a victim");
    replacer.validate();
}

void testK1Recency() {
    LRUKReplacer replacer(3, 1);
    accessAndRelease(replacer, 0);
    accessAndRelease(replacer, 1);
    replacer.recordAccess(0);
    require(replacer.evict() == 1, "K=1 did not evict least-recently used frame");
    require(replacer.evict() == 0, "K=1 remaining victim was incorrect");
    replacer.validate();
}

void testInfinityAndDeterministicTies() {
    LRUKReplacer replacer(4, 2);
    accessAndRelease(replacer, 2); // t1, infinity and earliest
    accessAndRelease(replacer, 1); // t2, infinity
    replacer.recordAccess(1);      // finite
    accessAndRelease(replacer, 0); // infinity, later
    require(replacer.evict() == 2, "infinity tie did not use earliest first access");
    require(replacer.evict() == 0, "infinity victim was not preferred over finite victim");

    LRUKReplacer frameTie(3, 3);
    frameTie.recordAccess(0);
    frameTie.recordAccess(1);
    // Equal timestamps cannot arise naturally; this verifies the deterministic one-frame case.
    frameTie.setEvictable(0, true);
    frameTie.setEvictable(1, true);
    require(frameTie.evict() == 0, "earlier infinity history was not selected");
    frameTie.validate();
}

void testFiniteDistanceAndAccessChange() {
    LRUKReplacer replacer(3, 2);
    replacer.recordAccess(0); // t1
    replacer.recordAccess(1); // t2
    replacer.recordAccess(0); // t3: frame 0 kth timestamp 1
    replacer.recordAccess(1); // t4: frame 1 kth timestamp 2
    replacer.setEvictable(0, true);
    replacer.setEvictable(1, true);
    require(replacer.victim() == 0, "maximum finite backward K-distance was incorrect");
    replacer.recordAccess(0); // history t3,t5: frame 1 is now older
    require(replacer.victim() == 1, "recordAccess did not update finite victim ordering");
    replacer.validate();
}

void testEvictabilityRemoveAndSize() {
    LRUKReplacer replacer(3, 3);
    replacer.recordAccess(0);
    replacer.recordAccess(1);
    replacer.setEvictable(0, true);
    require(replacer.size() == 1 && replacer.trackedSize() == 2,
            "LRU-K size accounting was incorrect");
    require(replacer.remove(0) && !replacer.remove(0), "LRU-K remove result changed");
    minidb::test::requireThrows<std::logic_error>(
        [&] { static_cast<void>(replacer.remove(1)); },
        "non-evictable frame removal was accepted");
    replacer.setEvictable(1, true);
    replacer.setEvictable(1, false);
    require(!replacer.evict().has_value(), "non-evictable frame was returned");
    replacer.setEvictable(1, true);
    require(replacer.evict() == 1 && replacer.size() == 0,
            "evictability toggle or size accounting failed");
    replacer.validate();
}

void testK2ScanResistanceTrace() {
    LRUKReplacer replacer(4, 2);
    // Frame 0 is reused; frames 1..3 model one-touch scan pages.
    replacer.recordAccess(0); // t1
    replacer.recordAccess(1); // t2
    replacer.recordAccess(0); // t3, finite history
    replacer.recordAccess(2); // t4
    replacer.recordAccess(3); // t5
    for (minidb::FrameId frame = 0; frame < 4; ++frame) {
        replacer.setEvictable(frame, true);
    }
    require(replacer.evict() == 1, "LRU-2 scan trace displaced reused hot frame first");
    require(replacer.evict() == 2, "LRU-2 scan trace did not order one-touch pages by age");
    require(replacer.evict() == 3, "LRU-2 scan trace did not retain finite-history page");
    require(replacer.evict() == 0, "LRU-2 hot page was not retained until last");
}

void testKGreaterThanTwoAndRepeatedAccess() {
    LRUKReplacer replacer(2, 4);
    for (int count = 0; count < 7; ++count) replacer.recordAccess(0);
    replacer.recordAccess(1);
    replacer.setEvictable(0, true);
    replacer.setEvictable(1, true);
    require(replacer.evict() == 1, "K>2 did not prefer fewer-than-K history");
    replacer.validate();
}

} // namespace

int main() {
    try {
        testConstructionAndEmpty();
        testK1Recency();
        testInfinityAndDeterministicTies();
        testFiniteDistanceAndAccessChange();
        testEvictabilityRemoveAndSize();
        testK2ScanResistanceTrace();
        testKGreaterThanTwoAndRepeatedAccess();
        std::cout << "LRU-K replacer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LRU-K replacer test failure: " << error.what() << '\n';
        return 1;
    }
}
