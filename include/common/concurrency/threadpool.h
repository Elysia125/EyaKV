#ifndef EYAKV_COMMON_THREADPOOL_H
#define EYAKV_COMMON_THREADPOOL_H

#include "common/base/export.h"
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

/**
 * @brief 高性能有界线程池 (生产者-消费者模型)
 *
 * 用于管理固定数量的工作线程和一个有界任务队列。
 * 支持超时防阻塞机制：当任务队列满时，提交者会根据配置等待特定时间，超时则拒绝。
 * 支持优雅停机与立即停机策略。
 */
class EYAKV_COMMON_API ThreadPool
{
public:
    /**
     * @brief 任务类型定义，使用 std::function 包装无参数无返回值的可调用对象
     */
    using Task = std::function<void()>;

    /**
     * @brief 线程池核心配置参数
     */
    struct Config
    {
        uint32_t thread_count;    ///< 工作线程的常驻数量
        uint32_t queue_size;      ///< 任务缓冲队列的最大容量
        uint32_t wait_timeout_ms; ///< 队列满时，生产者等待空闲槽位的超时时间 (毫秒)
    };

public:
    /**
     * @brief 构造函数，初始化并立即启动所有工作线程
     * @param config 线程池配置参数
     */
    explicit ThreadPool(const Config &config);

    /**
     * @brief 析构函数，默认触发优雅关闭，等待所有排队任务完成
     */
    ~ThreadPool();

    // 禁用拷贝与赋值操作，确保线程池单例或明确的所有权
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief 提交任务到线程池
     *
     * 如果队列未满，立即将任务加入队列；
     * 如果队列已满，则阻塞等待配置的 wait_timeout_ms 时间；
     *
     * @param task 要异步执行的任务（推荐使用 std::move 传入）
     * @return true 任务成功提交到队列
     * @return false 任务提交失败（由于队列满且超时，或线程池已被标记为停止）
     */
    [[nodiscard]] bool submit(Task task);

    /**
     * @brief 获取当前等待队列中的积压任务数量
     * @return uint32_t 队列中待处理的任务数
     */
    [[nodiscard]] uint32_t get_pending_task_count() const;

    /**
     * @brief 获取当前正在执行任务的活跃工作线程数量
     * @return uint32_t 正在忙碌的工作线程数
     */
    [[nodiscard]] uint32_t get_active_thread_count() const;

    /**
     * @brief 优雅停止线程池
     * 拒接所有新任务，但会等待队列中已有的任务全部执行完毕后再销毁线程。
     */
    void stop();

    /**
     * @brief 立即停止线程池
     * 拒接所有新任务，直接清空（丢弃）等待队列中的任务，并等待正在执行的任务完成后销毁线程。
     */
    void stop_immediately();

private:
    /**
     * @brief 工作线程生命周期循环
     * @param thread_id 用于内部日志追踪的线程虚拟编号
     */
    void worker_thread(uint32_t thread_id);

    /**
     * @brief 统一的关闭逻辑底层实现
     * @param immediate 是否为立即关闭模式
     */
    void shutdown(bool immediate);

private:
    std::vector<std::thread> workers_;          ///< 工作线程容器
    std::queue<Task> task_queue_;               ///< 任务缓冲队列
    
    mutable std::mutex queue_mutex_;            ///< 保护队列的互斥锁
    std::condition_variable queue_not_empty_;   ///< 队列非空条件变量 (唤醒消费者)
    std::condition_variable queue_not_full_;    ///< 队列未满条件变量 (唤醒生产者)
    
    std::atomic<bool> stop_{false};             ///< 线程池全局停止标记
    std::atomic<uint32_t> active_threads_{0};   ///< 当前正在执行任务的活跃线程数

    const uint32_t max_thread_count_;           ///< 最大工作线程数 (不可变优化)
    const uint32_t max_queue_size_;             ///< 队列最大容量 (不可变优化)
    const uint32_t wait_timeout_ms_;            ///< 提交等待超时时间 (不可变优化)
};

#endif // EYAKV_COMMON_THREADPOOL_H