#include "minidb/database_access_gate.hpp"
#include "concurrency_utils.hpp"

#include <atomic>
#include <iostream>
#include <latch>
#include <thread>
#include <vector>

using minidb::test::require;
using minidb::test::await;

int main() {
    try {
        minidb::DatabaseAccessGate gate;
        minidb::test::ThreadErrors errors;
        std::latch readersReady(8), releaseReaders(1), releaseWriter(1);
        std::vector<std::jthread> readers;
        for (int i = 0; i < 8; ++i) readers.emplace_back([&] { errors.run([&] {
            auto lease = gate.acquireShared(); readersReady.count_down(); releaseReaders.wait();
        }); });
        readersReady.wait();
        require(gate.stats().activeReaders == 8, "Shared leases did not coexist");
        std::atomic<bool> writerEntered{false}, lateReaderEntered{false};
        std::jthread writer([&] { errors.run([&] {
            auto lease = gate.acquireExclusive(); writerEntered = true; releaseWriter.wait();
        }); });
        await([&] { return gate.stats().waitingWriters == 1; }, "Writer did not wait behind readers");
        std::jthread lateReader([&] { errors.run([&] {
            auto lease = gate.acquireShared(); lateReaderEntered = true;
        }); });
        await([&] { return gate.stats().waitingReaders == 1; }, "Late reader bypassed waiting writer");
        require(!writerEntered && !lateReaderEntered, "Blocked owners entered early");
        releaseReaders.count_down();
        await([&] { return writerEntered.load(); }, "Released readers did not wake writer");
        require(!lateReaderEntered, "Writer did not exclude reader");
        releaseWriter.count_down();
        for (auto& reader : readers) reader.join();
        writer.join(); lateReader.join(); errors.rethrow();
        require(lateReaderEntered && gate.stats().peakConcurrentReaders == 8, "Gate reader accounting wrong");
        {
            auto shared = gate.acquireShared();
            minidb::test::requireThrows<std::logic_error>([&] { static_cast<void>(gate.acquireExclusive()); },
                "Gate silently upgraded a shared lease");
        }
        {
            auto exclusive = gate.acquireExclusive();
            std::atomic<bool> cancel{false}, cancelled{false};
            std::jthread waiter([&] { errors.run([&] {
                try { auto lease = gate.acquireExclusive([&] { return cancel.load(); }); }
                catch (const minidb::AccessCancelled&) { cancelled = true; }
            }); });
            await([&] { return gate.stats().waitingWriters == 1; }, "Second writer did not wait");
            cancel = true; waiter.join();
            require(cancelled && gate.stats().waitingWriters == 0, "Cancelled writer leaked waiter");
            std::jthread stopped([&] { errors.run([&] {
                minidb::test::requireThrows<minidb::AccessCancelled>(
                    [&] { static_cast<void>(gate.acquireShared()); }, "Shutdown did not cancel reader");
            }); });
            await([&] { return gate.stats().waitingReaders == 1; }, "Shutdown reader did not wait");
            gate.shutdown(); stopped.join();
            require(gate.stats().writerActive, "Shutdown released active owner prematurely");
        }
        errors.rethrow();
        require(!gate.stats().writerActive && gate.stats().activeReaders == 0, "Gate leaked leases");
        std::cout << "database_access_gate_test passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
