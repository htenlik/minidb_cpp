#pragma once

#include "test_utils.hpp"
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>

namespace minidb::test {
template<class Predicate>
void await(Predicate predicate, std::string_view message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::yield();
    }
}
class ThreadErrors {
    std::mutex mutex_;
    std::exception_ptr error_;
public:
    template<class Function> void run(Function&& function) noexcept {
        try { function(); }
        catch (...) { std::lock_guard lock(mutex_); if (!error_) error_ = std::current_exception(); }
    }
    void rethrow() { std::lock_guard lock(mutex_); if (error_) std::rethrow_exception(error_); }
};
} // namespace minidb::test
