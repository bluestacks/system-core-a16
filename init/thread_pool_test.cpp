#include "thread_pool.h"

#include <atomic>
#include <latch>

#include <gtest/gtest.h>

namespace android {
namespace init {

TEST(ThreadPoolTest, ImmediateStopWorks) {
    ThreadPool pool(4);
    // The pool should stop without any error.
    pool.Wait();
}

TEST(ThreadPoolTest, DoesNotStopWhenTaskQueueIsEmptyBeforeWait) {
    ThreadPool pool(4);
    std::latch finished(1);
    pool.Enqueue([&] { finished.count_down(); });

    // Wait for the first task to complete.
    finished.wait();

    // Now the queue is empty, but the pool is still running.

    bool executed = false;
    pool.Enqueue([&] { executed = true; });

    pool.Wait();
    // The second task should have been executed.
    EXPECT_TRUE(executed);
}

TEST(ThreadPoolTest, EnqueueAfterStopFails) {
    ThreadPool pool(4);
    bool executed = false;
    pool.Enqueue([&] { executed = true; });
    pool.Wait();
    EXPECT_TRUE(executed);
    // The pool is stopped, so it should crash when a new task is enqueued.
    EXPECT_DEATH(pool.Enqueue([] {}), "");
}

TEST(ThreadPoolTest, ThreadNumberDoesNotChangeAfterQueueIsEmpty) {
    ThreadPool pool(2);

    // Enqueue one task and wait for it to complete.
    std::latch finished(1);
    pool.Enqueue([&] { finished.count_down(); });
    finished.wait();

    // Now the queue is empty, but the pool is still running.

    // Enqueue two tasks, and check if the number of threads in the pool is still 2.
    std::latch completed(3);
    for (size_t i = 0; i < 2; ++i) {
        pool.Enqueue([&] { completed.arrive_and_wait(); });
    }
    completed.arrive_and_wait();
    // We would not reach here if the number of worker threads in the pool was not 2.

    pool.Wait();
}

class ThreadPoolForTest {
  public:
    void SetWaitCallbackForTest(ThreadPool& pool, std::function<void()> callback) {
        pool.wait_callback_for_test_ = std::move(callback);
    }
};

TEST(ThreadPoolTest, EnqueueTasksWhileStopping) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    std::latch started(1);
    std::latch cont(1);

    // Enqueue a task that will block, ensuring the pool has a busy thread.
    pool.Enqueue([&] {
        counter++;
        started.count_down();
        cont.wait();
    });

    // Wait for the first task to start.
    started.wait();

    ThreadPoolForTest t;
    t.SetWaitCallbackForTest(pool, [&] {
        // Now the thread pool is in State::Stopping.
        pool.Enqueue([&counter] { counter++; });
        // Unblock the first task.
        cont.count_down();
    });

    pool.Wait();

    // All tasks should have been executed.
    EXPECT_EQ(counter, 2);
}

}  // namespace init
}  // namespace android
