#include "minidb/catalog.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/table.hpp"
#include "test_utils.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t OPERATIONS_PER_SEED = 1500;
constexpr std::size_t REOPEN_CADENCE = 150;

std::uint64_t encodeRid(minidb::RecordId rid) {
    return (static_cast<std::uint64_t>(rid.pageId) << 16U) | rid.slotId;
}

minidb::Schema accountSchema() {
    return minidb::Schema::create({
        {"id", minidb::DataType::UINT32, false, true, 0},
        {"balance", minidb::DataType::INT64, false, false, 0},
        {"enabled", minidb::DataType::BOOLEAN, false, false, 0},
        {"note", minidb::DataType::VARCHAR, true, false, 3000},
    });
}

minidb::Schema eventSchema() {
    return minidb::Schema::create({
        {"message", minidb::DataType::VARCHAR, false, false, 256},
        {"important", minidb::DataType::BOOLEAN, false, false, 0},
    });
}

std::string randomString(std::mt19937_64& random, std::size_t maximum) {
    const auto selector = random() % 20U;
    std::size_t length = 0;
    if (selector == 0) {
        length = 1000 + random() % 1501U;
    } else if (selector < 4) {
        length = 129 + random() % 384U;
    } else {
        length = random() % 129U;
    }
    length = std::min(length, maximum);
    std::string value(length, '\0');
    for (std::size_t index = 0; index < length; ++index) {
        value[index] = static_cast<char>('a' + ((random() + index) % 26U));
    }
    return value;
}

minidb::RowValues randomAccountRow(std::mt19937_64& random, std::uint32_t key) {
    minidb::Value note = std::monostate{};
    if ((random() % 5U) != 0) {
        note = randomString(random, 3000);
    }
    return {
        key,
        static_cast<std::int64_t>(random()),
        (random() & 1U) != 0,
        std::move(note),
    };
}

void compareAccounts(
    minidb::Table& table,
    const std::map<std::uint32_t, minidb::RowValues>& expected) {
    minidb::test::require(table.size() == expected.size(),
                          "Random table size disagrees with oracle");
    std::map<std::uint32_t, minidb::RowValues> observed;
    for (const auto& row : table.scan()) {
        const auto key = std::get<std::uint32_t>(row.values[0]);
        minidb::test::require(observed.emplace(key, row.values).second,
                              "Random table scan produced duplicate primary key");
    }
    minidb::test::require(observed == expected,
                          "Random table scan contents disagree with oracle");
}

void compareEvents(
    minidb::Table& table,
    const std::map<std::uint64_t, minidb::RowValues>& expected) {
    minidb::test::require(table.size() == expected.size(),
                          "Cross-table event size disagrees with oracle");
    std::map<std::uint64_t, minidb::RowValues> observed;
    for (const auto& row : table.scan()) {
        observed.emplace(encodeRid(row.recordId), row.values);
    }
    minidb::test::require(observed == expected,
                          "Cross-table event contents disagree with oracle");
}

void runSeed(std::uint64_t seed) {
    minidb::test::TemporaryDatabase database("table_random_" + std::to_string(seed));
    std::mt19937_64 random(seed);
    std::map<std::uint32_t, minidb::RowValues> accounts;
    std::map<std::uint64_t, minidb::RowValues> events;
    minidb::TableId accountTableId = minidb::INVALID_TABLE_ID;
    minidb::TableId eventTableId = minidb::INVALID_TABLE_ID;

    for (std::size_t batchStart = 0; batchStart < OPERATIONS_PER_SEED;
         batchStart += REOPEN_CADENCE) {
        minidb::Pager pager(database.path().string());
        auto catalog = minidb::Catalog::openOrCreate(pager);
        if (accountTableId == minidb::INVALID_TABLE_ID) {
            auto accountTable = catalog.createTable("accounts", accountSchema());
            auto eventTable = catalog.createTable("events", eventSchema());
            accountTableId = accountTable.id();
            eventTableId = eventTable.id();
        }
        auto accountTable = catalog.openTable(accountTableId);
        auto eventTable = catalog.openTable(eventTableId);
        const auto batchEnd = std::min(batchStart + REOPEN_CADENCE, OPERATIONS_PER_SEED);

        for (std::size_t operation = batchStart; operation < batchEnd; ++operation) {
            const auto kind = random() % 6U;
            const auto key = static_cast<std::uint32_t>(random() % 501U);
            try {
                if (kind == 0) {
                    auto values = randomAccountRow(random, key);
                    const auto found = accounts.find(key);
                    if (found == accounts.end()) {
                        static_cast<void>(accountTable.insert(values));
                        accounts.emplace(key, std::move(values));
                    } else {
                        minidb::test::requireThrows<std::invalid_argument>(
                            [&] { static_cast<void>(accountTable.insert(values)); },
                            "Random duplicate primary key was accepted");
                    }
                } else if (kind == 1) {
                    const bool expected = accounts.erase(key) != 0;
                    minidb::test::require(accountTable.eraseByPrimaryKey(key) == expected,
                                          "Random erase result disagrees with oracle");
                } else if (kind == 2) {
                    const auto row = accountTable.findByPrimaryKey(key);
                    const auto expected = accounts.find(key);
                    minidb::test::require(row.has_value() == (expected != accounts.end()),
                                          "Random find presence disagrees with oracle");
                    if (row) {
                        minidb::test::require(row->values == expected->second,
                                              "Random find values disagree with oracle");
                    }
                } else if (kind == 3) {
                    const auto newKey = static_cast<std::uint32_t>(random() % 501U);
                    auto replacement = randomAccountRow(random, newKey);
                    const auto old = accounts.find(key);
                    const bool duplicateTarget = newKey != key && accounts.contains(newKey);
                    if (old == accounts.end()) {
                        minidb::test::require(
                            !accountTable.updateByPrimaryKey(key, replacement).has_value(),
                            "Random update created a missing source row");
                    } else if (duplicateTarget) {
                        minidb::test::requireThrows<std::invalid_argument>(
                            [&] {
                                static_cast<void>(
                                    accountTable.updateByPrimaryKey(key, replacement));
                            },
                            "Random update accepted duplicate target key");
                    } else {
                        minidb::test::require(
                            accountTable.updateByPrimaryKey(key, replacement).has_value(),
                            "Random update lost an existing source row");
                        accounts.erase(old);
                        accounts.emplace(newKey, std::move(replacement));
                    }
                } else if (kind == 4) {
                    compareAccounts(accountTable, accounts);
                } else {
                    const minidb::RowValues event{
                        randomString(random, 256),
                        (random() & 1U) != 0,
                    };
                    const auto rid = eventTable.insert(event);
                    events.emplace(encodeRid(rid), event);
                    if (events.size() > 20 && (random() % 3U) == 0) {
                        const auto victimIndex = static_cast<std::size_t>(random() % events.size());
                        auto victim = events.begin();
                        std::advance(victim, static_cast<std::ptrdiff_t>(victimIndex));
                        const minidb::RecordId victimRid{
                            static_cast<minidb::PageId>(victim->first >> 16U),
                            static_cast<minidb::SlotId>(victim->first & 0xFFFFU),
                        };
                        eventTable.erase(victimRid);
                        events.erase(victim);
                    }
                }

                if (operation % 50U == 0) {
                    compareAccounts(accountTable, accounts);
                    compareEvents(eventTable, events);
                    accountTable.validate();
                    eventTable.validate();
                    minidb::PageAllocator allocator(pager);
                    allocator.validate();
                }
                if (operation % 300U == 0) {
                    catalog.validate();
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    "seed=" + std::to_string(seed)
                    + " operation=" + std::to_string(operation)
                    + " kind=" + std::to_string(kind)
                    + " key=" + std::to_string(key)
                    + ": " + error.what());
            }
        }
        compareAccounts(accountTable, accounts);
        compareEvents(eventTable, events);
        accountTable.validate();
        eventTable.validate();
        catalog.validate();
        pager.flushAll();
    }

    minidb::Pager pager(database.path().string());
    auto catalog = minidb::Catalog::open(pager);
    auto accountTable = catalog.openTable("accounts");
    auto eventTable = catalog.openTable("events");
    compareAccounts(accountTable, accounts);
    compareEvents(eventTable, events);
    catalog.validate();
}

} // namespace

int main() {
    try {
        runSeed(0x5B000001ULL);
        runSeed(0x5B00C0DEULL);
        std::cout << "table_random_test passed (3000 operations, 2 seeds, 18 mid-run reopens)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "table_random_test failed: " << error.what() << '\n';
        return 1;
    }
}
