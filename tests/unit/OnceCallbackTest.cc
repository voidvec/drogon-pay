#include "utils/OnceCallback.h"
#include <drogon/drogon_test.h>
#include <atomic>
#include <thread>
#include <vector>

DROGON_TEST(OnceCallback_CallsOnlyOnce)
{
    int calls = 0;
    auto cb = pay::utils::makeOnceCallback<void(int)>([&calls](int value) { calls += value; });

    CHECK(cb.call(1));
    CHECK(!cb.call(1));
    CHECK(calls == 1);
    CHECK(!cb.valid());
}

DROGON_TEST(OnceCallback_EmptyCallbackIsNoOp)
{
    pay::utils::OnceCallback<void()> cb;
    CHECK(!cb.valid());
    CHECK(!cb.call());
}

DROGON_TEST(OnceCallback_SharedCopiesCallOnlyOnce)
{
    int calls = 0;
    auto cb = pay::utils::makeOnceCallback<void()>([&calls]() { ++calls; });
    auto copy = cb;

    CHECK(copy.call());
    CHECK(!cb.call());
    CHECK(calls == 1);
}

DROGON_TEST(OnceCallback_ConcurrentCallsOnlyOnce)
{
    std::atomic<int> calls{0};
    auto cb = pay::utils::makeOnceCallback<void()>([&calls]() { ++calls; });

    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i)
    {
        threads.emplace_back([cb]() { cb.call(); });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    CHECK(calls.load() == 1);
}

// P3-7.1: verify OnceCallback handles exceptions without double-firing
DROGON_TEST(OnceCallback_ExceptionDoesNotInvalidate)
{
    int calls = 0;
    auto cb = pay::utils::makeOnceCallback<void()>([&calls]() {
        ++calls;
        throw std::runtime_error("test exception");
    });

    try
    {
        cb.call();
    }
    catch (...)
    {
    }
    // After exception, callback should be marked as called (not valid anymore)
    CHECK(calls == 1);
    CHECK(!cb.valid());
    CHECK(!cb.call());
    CHECK(calls == 1);  // still 1 — second call prevented
}
