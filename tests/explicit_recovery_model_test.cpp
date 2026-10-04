#include "minidb/database_server.hpp"
#include "minidb/table.hpp"
#include "test_utils.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using minidb::test::require;
using Model = std::map<std::uint32_t, std::string>;
constexpr std::uint64_t SEEDS[]{0x12A20001, 0x12A20002, 0x12A20003, 0x12A20004};
constexpr std::size_t HISTORIES = 10;
constexpr std::size_t STATEMENTS = 512;

minidb::net::ServerConfig config(std::uint64_t seed) {
    minidb::net::ServerConfig result{"127.0.0.1", 0, 8, 3, 2};
    result.walUpdateMode = static_cast<minidb::WalUpdateMode>(seed % 3);
    result.walSegmentBytes = 32768;
    result.checkpointWalBytes = 0;
    return result;
}
void waitChild(pid_t pid, int expected, const std::string& context) {
    int status = 0;
    require(pid > 0 && ::waitpid(pid, &status, 0) == pid && WIFEXITED(status)
                && WEXITSTATUS(status) == expected,
            "Child failed: " + context + " status=" + std::to_string(status));
}
void compare(minidb::net::DatabaseServer& server, const Model& model) {
    Model actual;
    const auto selected = std::get<minidb::sql::SelectResult>(
        server.sqlEngine().execute("SELECT * FROM t"));
    for (const auto& row : selected.rows) {
        require(actual.emplace(std::get<std::uint32_t>(row[0]), std::get<std::string>(row[1])).second,
                "Duplicate recovered primary key");
    }
    require(actual == model, "Recovered tuples differ from transaction oracle");
    server.catalog().validate();
    server.catalog().openTable("t").validate();
    server.pageAllocator().validate();
    server.bufferPool().validate();
}
void run(std::uint64_t seed) {
    minidb::test::TemporaryDatabase database("explicit_recovery_model");
    const auto path = database.path().string();
    Model committed;
    std::mt19937_64 random(seed);
    {
        minidb::net::DatabaseServer server(path, config(seed));
        static_cast<void>(server.sqlEngine().execute(
            "CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(2000) NOT NULL)"));
    }
    for (std::size_t history = 0; history < HISTORIES; ++history) {
        const auto context = "seed=" + std::to_string(seed) + " history=" + std::to_string(history);
        Model candidate = committed;
        std::vector<std::string> statements;
        for (std::size_t operation = 0; operation < STATEMENTS; ++operation) {
            const auto key = static_cast<std::uint32_t>(random() % 48);
            const auto keyText = std::to_string(key);
            if (candidate.contains(key) && random() % 7 == 0) {
                statements.push_back("DELETE FROM t WHERE id = " + keyText);
                candidate.erase(key);
            } else {
                const auto size = static_cast<std::size_t>(1 + random() % 1800);
                const auto value = std::string(size, static_cast<char>('a' + random() % 26));
                statements.push_back(candidate.contains(key)
                    ? "UPDATE t SET value = '" + value + "' WHERE id = " + keyText
                    : "INSERT INTO t VALUES (" + keyText + ", '" + value + "')");
                candidate[key] = value;
            }
        }
        const auto outcome = history % 5;
        const auto pid = ::fork();
        if (pid < 0) throw std::runtime_error("fork failed");
        if (pid == 0) {
            std::size_t operation = 0;
            try {
                minidb::net::DatabaseServer server(path, config(seed));
                static_cast<void>(server.sqlEngine().execute("BEGIN"));
                for (; operation < statements.size(); ++operation) {
                    static_cast<void>(server.sqlEngine().execute(statements[operation]));
                    if (operation % 23 == 0) server.bufferPool().flushAll();
                }
                if (outcome == 0) {
                    static_cast<void>(server.sqlEngine().execute("COMMIT"));
                } else if (outcome == 2) {
                    static_cast<void>(server.sqlEngine().execute("ROLLBACK"));
                } else if (outcome == 3) {
                    ::setenv("MINIDB_FAILPOINT", "recovery_after_compensation_page_write", 1);
                    static_cast<void>(server.sqlEngine().execute("ROLLBACK"));
                    // The first empty-heap history can contain only appended data,
                    // but existing heap/catalog metadata still requires a CLR.
                    ::_exit(91);
                } else if (outcome == 4) {
                    ::setenv("MINIDB_FAILPOINT", "after_commit_sync", 1);
                    static_cast<void>(server.sqlEngine().execute("COMMIT"));
                    ::_exit(91);
                }
                ::_exit(86);
            } catch (const std::exception& error) {
                std::cerr << context << " operation=" << operation << ": " << error.what() << '\n';
                ::_exit(92);
            }
        }
        waitChild(pid, 86, context);
        if (outcome == 0 || outcome == 4) committed = std::move(candidate);
        // Repeated recovery interruptions must advance durable CLR progress.
        if (outcome == 1 || outcome == 3) {
            for (int restart = 0; restart < 2; ++restart) {
                const auto recoveryChild = ::fork();
                if (recoveryChild < 0) throw std::runtime_error("fork failed");
                if (recoveryChild == 0) {
                    ::setenv("MINIDB_FAILPOINT", "recovery_after_compensation_page_write", 1);
                    try {
                        minidb::net::DatabaseServer server(path, config(seed));
                        ::_exit(91);
                    } catch (...) { ::_exit(92); }
                }
                waitChild(recoveryChild, 86, context + " restart=" + std::to_string(restart));
            }
        }
        try {
            minidb::net::DatabaseServer server(path, config(seed));
            compare(server, committed);
            require(!server.sqlEngine().transactionManager().hasActiveExplicitTransaction(),
                    "Recovery left an active SQL scope");
            // Bound retained history while testing segmented reclamation after UNDO.
            static_cast<void>(server.checkpointManager().checkpoint());
        } catch (const std::exception& error) {
            throw std::runtime_error(context + ": " + error.what());
        }
    }
}
} // namespace

int main() {
    try {
        for (const auto seed : SEEDS) run(seed);
        std::cout << "explicit_recovery_model_test passed (20480 SQL mutations, 40 histories, "
                     "32 recovery interruptions; seeds 0x12A20001..0x12A20004)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "explicit_recovery_model_test failed: " << error.what() << '\n';
        return 1;
    }
}
