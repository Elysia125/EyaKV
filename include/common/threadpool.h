#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <future>
#include "common/export.h"

/**
 * @brief 线程池类，用于管理工作线程和任务队列
 *
 * 该线程池实现了一个生产者-消费者模式，主线程（IO线程）作为生产者提交任务，
 * 工作线程作为消费者从队列中获取任务并执行。线程池维护一个固定数量的工作线程
 * 和一个有界任务队列（等待队列），当队列满时会根据配置策略阻塞或拒绝新任务。
 */
class EYAKV_COMMON_API ThreadPool
{
public:
    /**
     * @brief 任务类型定义，使用std::function包装可调用对象
     */
    using Task = std::function<void()>;

    /**
     * @brief 线程池配置参数
     */
    struct Config
    {
        uint32_t thread_count;    // 工作线程数量
        uint32_t queue_size;      // 任务队列最大容量
        uint32_t wait_timeout_ms; // 任务提交时队列满时的等待超时时间（毫秒）
    };

public:
    /**
     * @brief 构造函数，初始化线程池
     *
     * @param config 线程池配置参数
     * @throws std::runtime_error 如果线程创建失败
     */
    explicit ThreadPool(const Config &config);

    /**
     * @brief 析构函数，优雅关闭线程池
     *
     * 会停止接受新任务，等待队列中的所有任务完成，然后销毁所有工作线程
     */
    ~ThreadPool();

    /**
     * @brief 提交任务到线程池
     *
     * 如果队列未满，立即将任务加入队列；如果队列已满，则根据配置的wait_timeout_ms
     * 等待指定时间，超时后返回false。任务会在某个空闲的工作线程上异步执行。
     *
     * @param task 要执行的任务（可调用对象）
     * @return true 任务成功提交到队列
     * @return false 任务提交失败（队列满且超时或线程池已停止）
     */
    bool submit(Task task);

    /**
     * @brief 获取当前等待队列中的任务数量
     *
     * @return uint32_t 队列中待处理的任务数
     */
    uint32_t get_pending_task_count() const;

    /**
     * @brief 获取当前活跃的工作线程数量
     *
     * @return uint32_t 正在执行任务的工作线程数
     */
    uint32_t get_active_thread_count() const;

    /**
     * @brief 停止线程池（优雅关闭）
     *
     * 设置停止标志，不再接受新任务，等待队列中所有任务完成后返回
     */
    void stop();

    /**
     * @brief 立即停止线程池（强制关闭）
     *
     * 设置停止标志，不再接受新任务，不等待队列中剩余任务完成
     */
    void stop_immediately();

private:
    /**
     * @brief 工作线程的主函数
     *
     * 每个工作线程都会执行此函数，持续从任务队列中获取任务并执行。
     * 线程会一直运行直到stop标志被设置且队列为空。
     */
    void worker_thread();

private:
    std::vector<std::thread> workers_;        // 工作线程容器
    std::queue<Task> task_queue_;             // 任务队列（等待队列）
    mutable std::mutex queue_mutex_;                  // 队列互斥锁
    std::condition_variable queue_not_empty_; // 队列非空条件变量（用于唤醒工作线程）
    std::condition_variable queue_not_full_;  // 队列未满条件变量（用于唤醒提交任务的线程）
    std::atomic<bool> stop_;                  // 停止标志
    std::atomic<uint32_t> active_threads_;    // 当前活跃的工作线程数
    uint32_t max_thread_count_;               // 最大工作线程数
    uint32_t max_queue_size_;                 // 队列最大容量
    uint32_t wait_timeout_ms_;                // 提交任务时的等待超时时间
};

#endif // THREADPOOL_H
