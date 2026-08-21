#include "minidb/bplus_tree.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ReferenceIndex = std::map<minidb::IndexKey, minidb::RecordId>;

minidb::RecordId makeRid(minidb::IndexKey key, std::uint32_t salt = 0) {
    return minidb::RecordId{
        1U + (key % 100'000U),
        static_cast<minidb::SlotId>((key + salt) % 50'000U),
    };
}

std::vector<minidb::IndexEntry> expectedEntries(const ReferenceIndex& reference) {
    std::vector<minidb::IndexEntry> entries;
    entries.reserve(reference.size());
    for (const auto& [key, recordId] : reference) {
        entries.push_back(minidb::IndexEntry{key, recordId});
    }
    return entries;
}

std::vector<minidb::IndexEntry> expectedRange(
    const ReferenceIndex& reference,
    minidb::IndexKey lower,
    minidb::IndexKey upper) {
    std::vector<minidb::IndexEntry> entries;
    if (lower > upper) {
        return entries;
    }
    for (auto position = reference.lower_bound(lower);
         position != reference.end() && position->first <= upper;
         ++position) {
        entries.push_back(minidb::IndexEntry{position->first, position->second});
    }
    return entries;
}

void requireMatches(const minidb::BPlusTree& tree, const ReferenceIndex& reference) {
    tree.validate();
    minidb::test::require(tree.size() == reference.size(), "B+ tree size differs from reference");
    minidb::test::require(
        tree.empty() == reference.empty(),
        "B+ tree empty state differs from reference");
    minidb::test::require(
        tree.scanAll() == expectedEntries(reference),
        "B+ tree ordered scan differs from reference");
    for (const auto& [key, recordId] : reference) {
        minidb::test::require(
            tree.find(key) == std::optional<minidb::RecordId>{recordId},
            "B+ tree exact lookup differs from reference");
    }
}

void testConfigurationAndEmptyTree() {
    minidb::test::requireThrows<std::invalid_argument>(
        [] { minidb::BPlusTree tree(2, 3); },
        "B+ tree accepted an undersized leaf capacity");
    minidb::test::requireThrows<std::invalid_argument>(
        [] { minidb::BPlusTree tree(3, 2); },
        "B+ tree accepted an undersized internal capacity");

    minidb::BPlusTree tree(3, 4);
    minidb::test::require(tree.empty(), "New B+ tree was not empty");
    minidb::test::require(tree.size() == 0, "New B+ tree size was not zero");
    minidb::test::require(tree.height() == 0, "Empty B+ tree had a nonzero height");
    minidb::test::require(!tree.find(1).has_value(), "Empty B+ tree found a key");
    minidb::test::require(tree.scanAll().empty(), "Empty B+ tree scan was not empty");
    minidb::test::require(tree.rangeScan(1, 9).empty(), "Empty range scan was not empty");
    minidb::test::require(!tree.erase(1), "Empty B+ tree erased a missing key");
    tree.validate();

    minidb::test::require(tree.leafMaxKeys() == 3, "Leaf capacity was not retained");
    minidb::test::require(tree.internalMaxKeys() == 4, "Internal capacity was not retained");
    minidb::test::require(tree.leafMinKeys() == 2, "Leaf minimum was not derived correctly");
    minidb::test::require(
        tree.internalMinKeys() == 2,
        "Internal minimum was not derived correctly");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(tree.insert(1, minidb::INVALID_RECORD_ID)); },
        "B+ tree accepted an invalid RID");
}

void testBasicLookupDuplicateAndRanges() {
    minidb::BPlusTree tree(3, 3);
    const auto original = makeRid(20, 1);
    minidb::test::require(tree.insert(20, original), "First insert was rejected");
    minidb::test::require(tree.find(20) == original, "Inserted RID was not found");
    minidb::test::require(!tree.find(19).has_value(), "Missing key was found");
    minidb::test::require(
        !tree.insert(20, makeRid(20, 2)),
        "Duplicate primary key was accepted");
    minidb::test::require(tree.find(20) == original, "Duplicate insert replaced the RID");
    minidb::test::require(tree.size() == 1, "Duplicate insert changed tree size");

    const auto minimum = std::numeric_limits<minidb::IndexKey>::min();
    const auto maximum = std::numeric_limits<minidb::IndexKey>::max();
    for (const auto key : {minimum, 10U, 30U, 40U, maximum}) {
        minidb::test::require(tree.insert(key, makeRid(key)), "Distinct insert was rejected");
        tree.validate();
    }

    const std::vector<minidb::IndexEntry> expectedBounded{
        {10, makeRid(10)}, {20, original}, {30, makeRid(30)}};
    minidb::test::require(
        tree.rangeScan(5, 35) == expectedBounded,
        "Range scan with absent bounds returned wrong entries");
    minidb::test::require(tree.rangeScan(40, 30).empty(), "Reversed range was not empty");
    minidb::test::require(
        tree.rangeScan(minimum, maximum) == tree.scanAll(),
        "Full-domain range did not match full scan");
    tree.validate();
}

void verifyInsertionOrder(std::vector<minidb::IndexKey> keys) {
    minidb::BPlusTree tree(3, 3);
    ReferenceIndex reference;
    for (const auto key : keys) {
        const auto recordId = makeRid(key, 17);
        minidb::test::require(tree.insert(key, recordId), "Ordered test insert was rejected");
        reference.emplace(key, recordId);
        tree.validate();
    }
    requireMatches(tree, reference);
    minidb::test::require(tree.height() >= 3, "Small capacities did not create three levels");
}

void testInsertionOrdersAndStructuralGrowth() {
    std::vector<minidb::IndexKey> keys(160);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        keys[index] = static_cast<minidb::IndexKey>(index);
    }
    verifyInsertionOrder(keys);

    std::reverse(keys.begin(), keys.end());
    verifyInsertionOrder(keys);

    std::mt19937 random(0x51A17U);
    std::shuffle(keys.begin(), keys.end(), random);
    verifyInsertionOrder(keys);
}

void testLeafBorrowAndMergeCases() {
    {
        minidb::BPlusTree tree(3, 3);
        for (const auto key : {1U, 2U, 3U, 4U, 5U}) {
            static_cast<void>(tree.insert(key, makeRid(key)));
        }
        minidb::test::require(tree.erase(1), "Right-borrow setup deletion failed");
        requireMatches(tree, ReferenceIndex{
            {2, makeRid(2)}, {3, makeRid(3)}, {4, makeRid(4)}, {5, makeRid(5)}});
    }
    {
        minidb::BPlusTree tree(3, 3);
        for (const auto key : {1U, 2U, 3U, 4U, 0U}) {
            static_cast<void>(tree.insert(key, makeRid(key)));
        }
        minidb::test::require(tree.erase(4), "Left-borrow setup deletion failed");
        requireMatches(tree, ReferenceIndex{
            {0, makeRid(0)}, {1, makeRid(1)}, {2, makeRid(2)}, {3, makeRid(3)}});
    }
    {
        minidb::BPlusTree tree(3, 3);
        for (const auto key : {1U, 2U, 3U, 4U}) {
            static_cast<void>(tree.insert(key, makeRid(key)));
        }
        minidb::test::require(tree.erase(1), "Leaf-merge setup deletion failed");
        minidb::test::require(tree.height() == 1, "Leaf merge did not shrink the root");
        requireMatches(tree, ReferenceIndex{
            {2, makeRid(2)}, {3, makeRid(3)}, {4, makeRid(4)}});
    }
}

void testRecursiveRebalancingRootShrinkAndRidStability() {
    minidb::BPlusTree tree(3, 3);
    ReferenceIndex reference;
    for (minidb::IndexKey key = 0; key < 240; ++key) {
        const auto recordId = makeRid(key, key * 7U);
        static_cast<void>(tree.insert(key, recordId));
        reference.emplace(key, recordId);
    }
    const auto initialHeight = tree.height();
    minidb::test::require(initialHeight >= 4, "Deletion test did not create a deep tree");

    for (minidb::IndexKey key = 0; key < 239; ++key) {
        minidb::test::require(tree.erase(key), "Existing key could not be erased");
        reference.erase(key);
        tree.validate();
        if (key % 19U == 0) {
            requireMatches(tree, reference);
        }
    }
    minidb::test::require(tree.height() == 1, "Root did not shrink to its final leaf");
    requireMatches(tree, reference);
    minidb::test::require(!tree.erase(42), "Missing key deletion reported success");

    minidb::test::require(tree.erase(239), "Final key could not be erased");
    minidb::test::require(tree.empty() && tree.height() == 0, "Final erase did not empty tree");
    tree.validate();

    const auto replacement = makeRid(77, 99);
    minidb::test::require(tree.insert(77, replacement), "Reinsert after empty failed");
    minidb::test::require(tree.find(77) == replacement, "Reinserted RID was not retained");
    tree.validate();
}

void runAdversarialOrder(bool reverseDeletion) {
    constexpr minidb::IndexKey KEY_COUNT = 300;
    minidb::BPlusTree tree(3, 4);
    ReferenceIndex reference;
    for (minidb::IndexKey key = 0; key < KEY_COUNT; ++key) {
        static_cast<void>(tree.insert(key, makeRid(key, 1)));
        reference.emplace(key, makeRid(key, 1));
        tree.validate();
    }
    for (minidb::IndexKey index = 0; index < KEY_COUNT; ++index) {
        const auto key = reverseDeletion ? KEY_COUNT - index - 1 : index;
        minidb::test::require(tree.erase(key), "Adversarial ordered erase failed");
        reference.erase(key);
        tree.validate();
    }
    requireMatches(tree, reference);
}

void testAdversarialMutations() {
    runAdversarialOrder(false);
    runAdversarialOrder(true);

    minidb::BPlusTree tree(4, 3);
    ReferenceIndex reference;
    constexpr minidb::IndexKey KEY_COUNT = 400;
    for (minidb::IndexKey key = 1; key < KEY_COUNT; key += 2) {
        static_cast<void>(tree.insert(key, makeRid(key, 2)));
        reference.emplace(key, makeRid(key, 2));
    }
    for (minidb::IndexKey key = 0; key < KEY_COUNT; key += 2) {
        static_cast<void>(tree.insert(key, makeRid(key, 2)));
        reference.emplace(key, makeRid(key, 2));
    }
    requireMatches(tree, reference);

    for (minidb::IndexKey key = 0; key < KEY_COUNT; key += 2) {
        static_cast<void>(tree.erase(key));
        reference.erase(key);
        tree.validate();
    }
    requireMatches(tree, reference);

    for (std::uint32_t cycle = 0; cycle < 3; ++cycle) {
        for (minidb::IndexKey key = 0; key < KEY_COUNT; key += 2) {
            const auto recordId = makeRid(key, 10 + cycle);
            static_cast<void>(tree.insert(key, recordId));
            reference.emplace(key, recordId);
        }
        for (minidb::IndexKey key = cycle; key < KEY_COUNT; key += 3) {
            static_cast<void>(tree.erase(key));
            reference.erase(key);
        }
        requireMatches(tree, reference);
    }
}

std::string operationContext(
    std::uint32_t seed,
    std::size_t operation,
    std::size_t leafCapacity,
    std::size_t internalCapacity,
    std::string_view action,
    minidb::IndexKey key) {
    std::ostringstream stream;
    stream << "seed=" << seed << " operation=" << operation
           << " capacities=" << leafCapacity << '/' << internalCapacity
           << " action=" << action << " key=" << key;
    return stream.str();
}

void runDifferentialTest(
    std::size_t leafCapacity,
    std::size_t internalCapacity,
    std::uint32_t seed) {
    constexpr std::size_t OPERATION_COUNT = 5'000;
    constexpr minidb::IndexKey KEY_DOMAIN = 1'024;
    minidb::BPlusTree tree(leafCapacity, internalCapacity);
    ReferenceIndex reference;
    std::mt19937 random(seed);

    for (std::size_t operation = 0; operation < OPERATION_COUNT; ++operation) {
        const auto key = static_cast<minidb::IndexKey>(random() % KEY_DOMAIN);
        const auto choice = random() % 100U;
        std::string_view action;
        try {
            if (choice < 35U) {
                action = "insert";
                const auto recordId = makeRid(key, static_cast<std::uint32_t>(operation));
                const bool actual = tree.insert(key, recordId);
                const bool expected = reference.emplace(key, recordId).second;
                minidb::test::require(actual == expected, "Insert result differs from std::map");
            } else if (choice < 60U) {
                action = "erase";
                const bool actual = tree.erase(key);
                const bool expected = reference.erase(key) != 0;
                minidb::test::require(actual == expected, "Erase result differs from std::map");
            } else if (choice < 80U) {
                action = "find";
                const auto actual = tree.find(key);
                const auto expected = reference.find(key);
                minidb::test::require(
                    actual.has_value() == (expected != reference.end()),
                    "Find presence differs from std::map");
                if (actual) {
                    minidb::test::require(*actual == expected->second, "Find RID differs from std::map");
                }
            } else {
                action = "rangeScan";
                const auto other = static_cast<minidb::IndexKey>(random() % KEY_DOMAIN);
                minidb::test::require(
                    tree.rangeScan(key, other) == expectedRange(reference, key, other),
                    "Range scan differs from std::map");
            }

            tree.validate();
            if (operation % 50 == 0) {
                requireMatches(tree, reference);
                const auto lower = static_cast<minidb::IndexKey>(random() % KEY_DOMAIN);
                const auto upper = static_cast<minidb::IndexKey>(random() % KEY_DOMAIN);
                minidb::test::require(
                    tree.rangeScan(lower, upper) == expectedRange(reference, lower, upper),
                    "Periodic range scan differs from std::map");
            }
        } catch (const std::exception& error) {
            throw std::runtime_error(
                operationContext(
                    seed, operation, leafCapacity, internalCapacity, action, key)
                + ": " + error.what());
        }
    }
    requireMatches(tree, reference);
}

void testRandomizedDifferentialOperations() {
    constexpr std::array<std::pair<std::size_t, std::size_t>, 4> CONFIGURATIONS{{
        {3, 3}, {4, 4}, {5, 4}, {8, 6},
    }};
    constexpr std::array<std::uint32_t, 2> SEEDS{{0xC0FFEEU, 0x5EED1234U}};
    for (const auto& [leafCapacity, internalCapacity] : CONFIGURATIONS) {
        for (const auto seed : SEEDS) {
            runDifferentialTest(leafCapacity, internalCapacity, seed);
        }
    }
}

} // namespace

int main() {
    try {
        testConfigurationAndEmptyTree();
        testBasicLookupDuplicateAndRanges();
        testInsertionOrdersAndStructuralGrowth();
        testLeafBorrowAndMergeCases();
        testRecursiveRebalancingRootShrinkAndRidStability();
        testAdversarialMutations();
        testRandomizedDifferentialOperations();
        std::cout << "bplus_tree_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bplus_tree_test failed: " << error.what() << '\n';
        return 1;
    }
}
