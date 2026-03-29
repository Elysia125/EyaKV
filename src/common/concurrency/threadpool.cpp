#include "common/concurrency/threadpool.h"
#include "logger/logger.h"
#include <chrono>

ThreadPool::ThreadPool(const Config &config)
    : max_thread_count_(config.thread_count),
      max_queue_size_(config.queue_size),
      wait_timeout_ms_(config.wait_timeout_ms)
{
    LOG_INFO("Initializing ThreadPool with %u threads, max queue size %u, timeout %u ms",
             max_thread_count_, max_queue_size_, wait_timeout_ms_);

    workers_.reserve(max_thread_count_);
    for (uint32_t i = 0; i < max_thread_count_; ++i)
    {
        // 将虚拟序号传递给线程，方便日志追踪区分不同的 worker
        workers_.emplace_back(&ThreadPool::worker_thread, this, i);
    }

    LOG_INFO("ThreadPool initialized successfully with %zu worker threads", workers_.size());
}

ThreadPool::~ThreadPool()
{
    // 默认析构时走优雅停止流程
    stop();
    LOG_INFO("ThreadPool destroyed");
}

bool ThreadPool::submit(Task task)
{
    // 快速拦截：若已停机，直接拒接
    if (stop_.load(std::memory_order_acquire))
    {
        LOG_WARN("ThreadPool is stopped, rejecting new task");
        return false;
    }

    std::unique_lock<std::mutex> lock(queue_mutex_);

    // 当队列达到上限时的阻塞与超时判定
    if (task_queue_.size() >= max_queue_size_)
    {
        LOG_WARN("Task queue is full (%zu/%u), waiting for available slot...",
                 task_queue_.size(), max_queue_size_);

        bool wait_result = queue_not_full_.wait_for(
            lock,
            std::chrono::milliseconds(wait_timeout_ms_),
            [this]()
            {
                return task_queue_.size() < max_queue_size_ || stop_.load(std::memory_order_acquire);
            });

        if (!wait_result || stop_.load(std::memory_order_acquire))
        {
            if (stop_.load(std::memory_order_acquire))
            {
                LOG_WARN("ThreadPool was stopped during waiting, rejecting task");
            }
            else
            {
                LOG_WARN("Task queue wait timeout after %u ms, rejecting task", wait_timeout_ms_);
            }
            return false;
        }
    }

    // 利用 std::move 减少 std::function 的拷贝开销
    task_queue_.emplace(std::move(task));

    LOG_DEBUG("Task submitted to queue (queue size: %zu/%u)",
              task_queue_.size(), max_queue_size_);

    // 释放锁后再唤醒消费者，避免消费者醒来后直接阻塞在互斥锁上 (避免 "Hurry up and wait" 现象)
    lock.unlock();
    queue_not_empty_.notify_one();

    return true;
}

uint32_t ThreadPool::get_pending_task_count() const
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return static_cast<uint32_t>(task_queue_.size());
}

uint32_t ThreadPool::get_active_thread_count() const
{
    return active_threads_.load(std::memory_order_relaxed);
}

void ThreadPool::stop()
{
    shutdown(false);
}

void ThreadPool::stop_immediately()
{
    shutdown(true);
}

void ThreadPool::shutdown(bool immediate)
{
    // 使用 CAS 保证关闭逻辑只会被执行一次
    bool expected = false;
    if (!stop_.compare_exchange_strong(expected, true, std::memory_order_release))
    {
        return; // 已经被其他线程关闭过
    }

    LOG_INFO(immediate ? "ThreadPool stopping immediately..." : "ThreadPool stopping gracefully...");

    if (immediate)
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        uint32_t discarded_tasks = static_cast<uint32_t>(task_queue_.size());

        // O(1) 复杂度极速清空队列：与一个空队列进行交换，省去逐个 pop 的开销
        std::queue<Task> empty_queue;
        std::swap(task_queue_, empty_queue);

        if (discarded_tasks > 0)
        {
            LOG_WARN("Discarded %u pending tasks during immediate shutdown", discarded_tasks);
        }
    }

    // 广播唤醒所有被条件变量挂起的工作线程和提交线程
    queue_not_empty_.notify_all();
    queue_not_full_.notify_all();

    for (auto &worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    workers_.clear();
    LOG_INFO(immediate ? "ThreadPool stopped immediately" : "ThreadPool stopped gracefully");
}

void ThreadPool::worker_thread(uint32_t thread_id)
{
    LOG_DEBUG("Worker thread [%u] started", thread_id);

    // RAII 辅助工具类：自动管理活跃线程的数量增减，做到绝对的异常安全
    struct ActiveThreadGuard
    {
        std::atomic<uint32_t> &active_counter_;
        ActiveThreadGuard(std::atomic<uint32_t> &counter) : active_counter_(counter)
        {
            active_counter_.fetch_add(1, std::memory_order_relaxed);
        }
        ~ActiveThreadGuard()
        {
            active_counter_.fetch_sub(1, std::memory_order_relaxed);
        }
    };

    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_not_empty_.wait(
                lock,
                [this]()
                {
                    return !task_queue_.empty() || stop_.load(std::memory_order_acquire);
                });

            // 如果已收到停止信号，且队列已被处理完毕（或被立即清空），则退出工作循环
            if (stop_.load(std::memory_order_acquire) && task_queue_.empty())
            {
                LOG_DEBUG("Worker thread [%u] exiting (stop signal received)", thread_id);
                break;
            }

            if (!task_queue_.empty())
            {
                task = std::move(task_queue_.front());
                task_queue_.pop();

                // 释放槽位后唤醒可能正在等待提交的生产者
                lock.unlock();
                queue_not_full_.notify_one();
            }
            else
            {
                continue;
            }
        }

        // 无锁执行任务阶段
        if (task)
        {
            // 利用 RAII 守卫保证即使抛出未知异常，活跃线程数也会正确递减
            ActiveThreadGuard guard(active_threads_);

            LOG_DEBUG("Executing task on worker [%u] (active threads: %u)",
                      thread_id, active_threads_.load(std::memory_order_relaxed));

            try
            {
                task();
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("Task execution exception on worker [%u]: %s", thread_id, e.what());
            }
            catch (...)
            {
                LOG_ERROR("Unknown exception during task execution on worker [%u]", thread_id);
            }

            LOG_DEBUG("Task completed on worker [%u]", thread_id);
        }
    }

    LOG_DEBUG("Worker thread [%u] terminated", thread_id);
}