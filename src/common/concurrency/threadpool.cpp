#include "common/concurrency/threadpool.h"
#include "logger/logger.h"
#include <chrono>
ThreadPool::ThreadPool(const Config &config)
    : stop_(false),
      active_threads_(0),
      max_thread_count_(config.thread_count),
      max_queue_size_(config.queue_size),
      wait_timeout_ms_(config.wait_timeout_ms)
{
    LOG_INFO("Initializing ThreadPool with %d threads and queue size %d",
             max_thread_count_, max_queue_size_);

    // 创建并启动所有工作线程
    for (uint32_t i = 0; i < max_thread_count_; ++i)
    {
        workers_.emplace_back(&ThreadPool::worker_thread, this);
        LOG_DEBUG("Worker thread %d created", i);
    }

    LOG_INFO("ThreadPool initialized successfully with %d worker threads", workers_.size());
}

ThreadPool::~ThreadPool()
{
    // 调用优雅停止
    stop();

    // 等待所有工作线程结束
    for (auto &worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    workers_.clear();
    LOG_INFO("ThreadPool destroyed");
}

bool ThreadPool::submit(Task task)
{
    // 如果线程池已停止，拒绝任务
    if (stop_.load())
    {
        LOG_WARN("ThreadPool is stopped, rejecting new task");
        return false;
    }

    std::unique_lock<std::mutex> lock(queue_mutex_);

    // 如果队列已满，等待直到队列有空间或超时
    if (task_queue_.size() >= max_queue_size_)
    {
        LOG_WARN("Task queue is full (%d/%d), waiting for available slot...",
                 task_queue_.size(), max_queue_size_);

        // 等待队列非空条件变量，超时时间为配置的wait_timeout_ms
        bool wait_result = queue_not_full_.wait_for(
            lock,
            std::chrono::milliseconds(wait_timeout_ms_),
            [this]()
            {
                // 条件：队列未满 或 线程池已停止
                return task_queue_.size() < max_queue_size_ || stop_.load();
            });

        // 如果超时或线程池停止，拒绝任务
        if (!wait_result || stop_.load())
        {
            if (stop_.load())
            {
                LOG_WARN("ThreadPool is stopped during wait, rejecting task");
            }
            else
            {
                LOG_WARN("Task queue wait timeout after %d ms, rejecting task",
                         wait_timeout_ms_);
            }
            return false;
        }
    }

    // 将任务加入队列
    task_queue_.push(task);
    LOG_DEBUG("Task submitted to queue (queue size: %d/%d)",
              task_queue_.size(), max_queue_size_);

    // 通知一个工作线程有新任务
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
    return active_threads_.load();
}

void ThreadPool::stop()
{
    // 设置停止标志，不再接受新任务
    bool expected = false;
    if (stop_.compare_exchange_strong(expected, true))
    {
        LOG_INFO("ThreadPool stopping gracefully...");

        // 唤醒所有工作线程，让它们检查停止标志
        queue_not_empty_.notify_all();
        queue_not_full_.notify_all();

        // 等待所有工作线程结束
        for (auto &worker : workers_)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        workers_.clear();
        LOG_INFO("ThreadPool stopped gracefully");
    }
}

void ThreadPool::stop_immediately()
{
    // 设置停止标志，不再接受新任务
    bool expected = false;
    if (stop_.compare_exchange_strong(expected, true))
    {
        LOG_WARN("ThreadPool stopping immediately...");

        // 清空任务队列（丢弃未执行的任务）
        std::unique_lock<std::mutex> lock(queue_mutex_);
        uint32_t discarded_tasks = static_cast<uint32_t>(task_queue_.size());
        while (!task_queue_.empty())
        {
            task_queue_.pop();
        }
        lock.unlock();

        if (discarded_tasks > 0)
        {
            LOG_WARN("Discarded %d pending tasks during immediate shutdown",
                     discarded_tasks);
        }

        // 唤醒所有工作线程，让它们检查停止标志
        queue_not_empty_.notify_all();
        queue_not_full_.notify_all();

        // 等待所有工作线程结束
        for (auto &worker : workers_)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        workers_.clear();
        LOG_WARN("ThreadPool stopped immediately");
    }
}

void ThreadPool::worker_thread()
{
    LOG_DEBUG("Worker thread started");

    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            // 等待队列非空或停止标志
            queue_not_empty_.wait(
                lock,
                [this]()
                {
                    // 条件：队列非空 或 线程池已停止
                    return !task_queue_.empty() || stop_.load();
                });

            // 检查停止标志：如果已停止且队列为空，则退出线程
            if (stop_.load() && task_queue_.empty())
            {
                LOG_DEBUG("Worker thread exiting (stop signal)");
                break;
            }

            // 从队列中获取任务
            if (!task_queue_.empty())
            {
                task = std::move(task_queue_.front());
                task_queue_.pop();

                // 通知可能等待队列空间的提交线程
                lock.unlock();
                queue_not_full_.notify_one();
            }
            else
            {
                // 队列为空，继续等待
                continue;
            }
        }

        // 执行任务（在锁外执行，允许并发）
        if (task)
        {
            active_threads_++; // 增加活跃线程计数
            LOG_DEBUG("Executing task (active threads: %d)",
                      active_threads_.load());

            try
            {
                task(); // 执行任务
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("Task execution exception: %s", e.what());
            }
            catch (...)
            {
                LOG_ERROR("Unknown exception during task execution");
            }

            active_threads_--; // 减少活跃线程计数
            LOG_DEBUG("Task completed (active threads: %d)",
                      active_threads_.load());
        }
    }

    LOG_DEBUG("Worker thread terminated");
}
