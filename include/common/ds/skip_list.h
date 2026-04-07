#ifndef SKIP_LIST_H
#define SKIP_LIST_H

#include <iostream>
#include <string>
#include <vector>
#include <random>
#include <ctime>
#include <chrono>
#include <cstring>
#include <atomic>
#include <optional>
#include <functional>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif

/// 默认跳表最大层级
#define DEFAULT_MAX_LEVEL 16
/// 默认跳表层级提升概率
#define DEFAULT_PROBABILITY 0.5

/**
 * @brief 跳表节点结构体，支持无锁并发读和 SeqLock 保护的 Value 更新。
 *
 * @tparam K 键的类型
 * @tparam V 值的类型
 */
template <typename K, typename V>
struct SkipListNode
{
    /// 节点的键（不可变）
    const K key;

    /// 节点的值（可能被并发更新，需通过 get_value/set_value 访问以防止撕裂读）
    V value;

    /**
     * @brief 顺序锁 (SeqLock)，解决 Memtable 无锁读时 Value 更新的“撕裂读(Torn Read)”问题。
     * - 偶数：稳定状态，数据可安全读取。
     * - 奇数：写入状态，写线程正在修改 Value，读线程需等待（自旋）。
     */
    std::atomic<uint32_t> seq_{0};

    /**
     * @brief 原子指针数组，保证并发读遍历时的安全性。
     * 数组大小等于该节点的 level，使得读线程在遍历时不会读到撕裂的指针。
     */
    std::atomic<SkipListNode<K, V> *> *next;

    /// 记录当前节点的层级
    int level;

    /**
     * @brief 构造函数，初始化跳表节点。
     *
     * @param key 键
     * @param value 值
     * @param level 该节点的高度（层级）
     */
    SkipListNode(const K &key, const V &value, int level) : key(key), value(value), level(level)
    {
        next = new std::atomic<SkipListNode<K, V> *>[level];
        for (int i = 0; i < level; ++i)
        {
            next[i].store(nullptr, std::memory_order_relaxed);
        }
    }

    /**
     * @brief 析构函数，释放原子指针数组占用的内存。
     */
    ~SkipListNode()
    {
        delete[] next;
    }

    // 禁用拷贝与赋值操作，确保原子变量和动态内存的安全
    SkipListNode(const SkipListNode &) = delete;
    SkipListNode &operator=(const SkipListNode &) = delete;
    /**
     * @brief 安全地无锁读取 Value（读端 SeqLock）。
     *
     * 读线程会检查 seq_ 锁：
     * 如果为奇数，说明写线程正在修改，主动让出 CPU 重试；
     * 如果为偶数，读取 value，并再次校验 seq_ 是否发生改变，确保读到的数据完整一致。
     *
     * @return V 完整且一致的 Value 拷贝
     */
    V get_value() const
    {
        V val;
        uint32_t seq;
        do
        {
            seq = seq_.load(std::memory_order_acquire);
            if (seq & 1)
            { // 奇数：写入中
                std::this_thread::yield();
                continue;
            }
            val = value;
            // 确保 val 的读取在下一次 seq_ 校验之前完成
            std::atomic_thread_fence(std::memory_order_acquire);
        } while (seq != seq_.load(std::memory_order_relaxed));
        return val;
    }

    /**
     * @brief 安全地无锁写入 Value（写端 SeqLock）。
     *
     * 将 seq_ 加 1 变为奇数（独占/修改中），完成修改后，再将 seq_ 加 1 变为偶数（释放）。
     * 配合 memory_order_release 保证修改对其他线程立刻可见。
     *
     * @param val 要更新的新 Value
     */
    void set_value(const V &val)
    {
        seq_.fetch_add(1, std::memory_order_release);
        value = val;
        seq_.fetch_add(1, std::memory_order_release);
    }
};

/**
 * @brief 高性能并发跳表实现，兼容 Memtable（无锁读+逻辑删除）和 ZSET（互斥锁+物理删除）。
 *
 *
 * @tparam K 键的类型
 * @tparam V 值的类型
 */
template <typename K, typename V>
class SkipList
{
private:
    /// 跳表允许的最大层级限制
    size_t MAX_LEVEL;
    /// 节点层数向上提升的概率（默认 0.5）
    double PROBABILITY;

    /// 当前跳表中节点的最大层级（原子操作，供并发读使用）
    std::atomic<int> current_level_{1};
    /// 头节点（哨兵节点，不存储实际数据）
    SkipListNode<K, V> *head_;
    /// 跳表当前包含的有效节点数量
    std::atomic<size_t> size_{0};
    /// 跳表当前估算的内存占用（字节），用于触发 Flush
    std::atomic<size_t> current_size_{0};

    /// 自定义键比较函数（-1: a<b, 0: a==b, 1: a>b）
    int (*compare_func_)(const K &, const K &);
    /// 自定义键内存大小计算函数
    size_t (*calculate_key_size_func_)(const K &);
    /// 自定义值内存大小计算函数
    size_t (*calculate_value_size_func_)(const V &);

    /**
     * @brief 随机生成新节点的层数。
     * @return int 新节点层数，范围 [1, MAX_LEVEL]
     */
    int random_level()
    {
        int level = 1;
        while (generate_random_01() < PROBABILITY && level < MAX_LEVEL)
            level++;
        return level;
    }

    /**
     * @brief 生成[0.0, 1.0) 之间的均匀分布随机数，线程安全。
     * @return double 随机值
     */
    double generate_random_01()
    {
        static std::mt19937 engine([]() -> uint32_t
                                   {
            std::random_device rd;
            if (rd.entropy() > 0) return rd();
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count(); }());
        static std::uniform_real_distribution<double> dist(0.0, 1.0);
        return dist(engine);
    }

    /**
     * @brief 初始化跳表的基础内存占用估算（包含哨兵节点）。
     */
    void init_current_size()
    {
        current_size_.store(calculate_key_size_func_(K()) + calculate_value_size_func_(V()) + sizeof(std::atomic<SkipListNode<K, V> *> *) * MAX_LEVEL, std::memory_order_relaxed);
    }

public:
    /**
     * @brief 构造函数，创建跳表实例。
     *
     * @param skiplist_max_level 最大层级限制
     * @param skiplist_probability 提升概率
     * @param compare_func 可选的键比较函数
     * @param calculate_key_size_func 可选的键大小计算函数
     * @param calculate_value_size_func 可选的值大小计算函数
     */
    SkipList(const size_t &skiplist_max_level = DEFAULT_MAX_LEVEL,
             const double &skiplist_probability = DEFAULT_PROBABILITY,
             std::optional<int (*)(const K &, const K &)> compare_func = std::nullopt,
             std::optional<size_t (*)(const K &)> calculate_key_size_func = std::nullopt,
             std::optional<size_t (*)(const V &)> calculate_value_size_func = std::nullopt)
        : MAX_LEVEL(skiplist_max_level),
          PROBABILITY(skiplist_probability)
    {
        compare_func_ = compare_func.value_or([](const K &a, const K &b)
                                              { return (a < b) ? -1 : ((a > b) ? 1 : 0); });
        calculate_key_size_func_ = calculate_key_size_func.value_or([](const K &key)
                                                                    { return sizeof(key); });
        calculate_value_size_func_ = calculate_value_size_func.value_or([](const V &value)
                                                                        { return sizeof(value); });

        head_ = new SkipListNode<K, V>(K(), V(), MAX_LEVEL);
        init_current_size();
    }

    /**
     * @brief 析构函数，释放所有节点及哨兵节点的内存。
     */
    ~SkipList()
    {
        clear();
        delete head_;
    }
    // 拷贝时确保无其他线程的写操作
    /**
     * @brief 拷贝构造函数
     * 深拷贝所有节点，并重建层级指针。
     */
    SkipList(const SkipList &other)
        : MAX_LEVEL(other.MAX_LEVEL),
          PROBABILITY(other.PROBABILITY)
    {
        compare_func_ = other.compare_func_;
        calculate_key_size_func_ = other.calculate_key_size_func_;
        calculate_value_size_func_ = other.calculate_value_size_func_;

        head_ = new SkipListNode<K, V>(K(), V(), MAX_LEVEL);
        current_level_.store(other.current_level_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        size_.store(other.size_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        current_size_.store(other.current_size_.load(std::memory_order_relaxed), std::memory_order_relaxed);

        // 记录每一层当前构建到的尾节点，方便后续节点的接入
        std::vector<SkipListNode<K, V> *> tail_nodes(MAX_LEVEL, head_);

        SkipListNode<K, V> *current = other.head_->next[0].load(std::memory_order_acquire);
        while (current != nullptr)
        {
            int lvl = current->level;
            // 拷贝节点键值和高度
            SkipListNode<K, V> *new_node = new SkipListNode<K, V>(current->key, current->get_value(), lvl);

            // 自底向上链接到新跳表中
            for (int i = 0; i < lvl; ++i)
            {
                tail_nodes[i]->next[i].store(new_node, std::memory_order_relaxed);
                tail_nodes[i] = new_node;
            }
            current = current->next[0].load(std::memory_order_acquire);
        }
    }

    /**
     * @brief 拷贝赋值操作符
     */
    SkipList &operator=(const SkipList &other)
    {
        if (this == &other)
            return *this; // 防止自赋值

        clear();      // 清空当前跳表数据
        delete head_; // 释放当前的头节点

        // 拷贝属性
        MAX_LEVEL = other.MAX_LEVEL;
        PROBABILITY = other.PROBABILITY;
        compare_func_ = other.compare_func_;
        calculate_key_size_func_ = other.calculate_key_size_func_;
        calculate_value_size_func_ = other.calculate_value_size_func_;

        // 重建基础状态
        head_ = new SkipListNode<K, V>(K(), V(), MAX_LEVEL);
        current_level_.store(other.current_level_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        size_.store(other.size_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        current_size_.store(other.current_size_.load(std::memory_order_relaxed), std::memory_order_relaxed);

        std::vector<SkipListNode<K, V> *> tail_nodes(MAX_LEVEL, head_);
        SkipListNode<K, V> *current = other.head_->next[0].load(std::memory_order_acquire);

        while (current != nullptr)
        {
            int lvl = current->level;
            SkipListNode<K, V> *new_node = new SkipListNode<K, V>(current->key, current->get_value(), lvl);
            for (int i = 0; i < lvl; ++i)
            {
                tail_nodes[i]->next[i].store(new_node, std::memory_order_relaxed);
                tail_nodes[i] = new_node;
            }
            current = current->next[0].load(std::memory_order_acquire);
        }

        return *this;
    }

    /**
     * @brief 移动构造函数
     * 窃取源跳表的资源，并将源跳表置为合法的空状态。
     */
    SkipList(SkipList &&other) noexcept
        : MAX_LEVEL(other.MAX_LEVEL),
          PROBABILITY(other.PROBABILITY)
    {
        compare_func_ = other.compare_func_;
        calculate_key_size_func_ = other.calculate_key_size_func_;
        calculate_value_size_func_ = other.calculate_value_size_func_;

        // 窃取资源
        head_ = other.head_;
        current_level_.store(other.current_level_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        size_.store(other.size_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        current_size_.store(other.current_size_.load(std::memory_order_relaxed), std::memory_order_relaxed);

        // 重置源跳表
        other.head_ = new SkipListNode<K, V>(K(), V(), other.MAX_LEVEL);
        other.current_level_.store(1, std::memory_order_relaxed);
        other.size_.store(0, std::memory_order_relaxed);
        other.init_current_size();
    }

    /**
     * @brief 移动赋值操作符
     */
    SkipList &operator=(SkipList &&other) noexcept
    {
        if (this == &other)
            return *this;

        clear();
        delete head_;

        MAX_LEVEL = other.MAX_LEVEL;
        PROBABILITY = other.PROBABILITY;
        compare_func_ = other.compare_func_;
        calculate_key_size_func_ = other.calculate_key_size_func_;
        calculate_value_size_func_ = other.calculate_value_size_func_;

        // 窃取资源
        head_ = other.head_;
        current_level_.store(other.current_level_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        size_.store(other.size_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        current_size_.store(other.current_size_.load(std::memory_order_relaxed), std::memory_order_relaxed);

        // 重置源跳表
        other.head_ = new SkipListNode<K, V>(K(), V(), other.MAX_LEVEL);
        other.current_level_.store(1, std::memory_order_relaxed);
        other.size_.store(0, std::memory_order_relaxed);
        other.init_current_size();

        return *this;
    }

    /**
     * @brief 插入或更新键值对（支持与其他写线程互斥下的无锁并发读）。
     *
     * 如果键存在，则使用 SeqLock 安全更新 Value；
     * 如果键不存在，则插入新节点。新节点的 `next` 指针从底向上构建，保证读取端的一致性。
     *
     * @param key 要插入的键
     * @param value 要插入的值
     */
    void insert(const K &key, const V &value)
    {
        size_t new_key_size = calculate_key_size_func_(key);
        size_t new_value_size = calculate_value_size_func_(value);

        SkipListNode<K, V> *current = head_;
        std::vector<SkipListNode<K, V> *> update(MAX_LEVEL, nullptr);

        int curr_lvl = current_level_.load(std::memory_order_acquire);
        for (int i = curr_lvl - 1; i >= 0; i--)
        {
            SkipListNode<K, V> *next_node = current->next[i].load(std::memory_order_acquire);
            while (next_node != nullptr && compare_func_(next_node->key, key) < 0)
            {
                current = next_node;
                next_node = current->next[i].load(std::memory_order_acquire);
            }
            update[i] = current;
        }

        current = current->next[0].load(std::memory_order_acquire);

        // Key 存在，无锁安全更新 Value
        if (current != nullptr && compare_func_(current->key, key) == 0)
        {
            size_t old_value_size = calculate_value_size_func_(current->get_value());
            current->set_value(value); // 内部含有 SeqLock
            current_size_.fetch_add(new_value_size - old_value_size, std::memory_order_relaxed);
            return;
        }

        // Key 不存在，准备插入
        int level = random_level();
        if (level > curr_lvl)
        {
            for (int i = curr_lvl; i < level; i++)
                update[i] = head_;
            current_level_.store(level, std::memory_order_release);
        }

        SkipListNode<K, V> *new_node = new SkipListNode<K, V>(key, value, level);

        // 1. 初始化新节点指针（此时新节点对外部读线程不可见）
        for (int i = 0; i < level; i++)
        {
            new_node->next[i].store(update[i]->next[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }

        // 2. 自底向上链接前驱节点，配合 release 语义，保证外部一旦看见高层指针，底层也必然已就绪
        for (int i = 0; i < level; i++)
        {
            update[i]->next[i].store(new_node, std::memory_order_release);
        }

        size_.fetch_add(1, std::memory_order_relaxed);
        current_size_.fetch_add(new_key_size + new_value_size + sizeof(std::atomic<SkipListNode<K, V> *>) * level + sizeof(SkipListNode<K, V>), std::memory_order_relaxed);
    }

    /**
     * @brief 无锁获取指定键的值。
     *
     * 支持与其他写入操作（insert/handle_value）完全并发执行。
     *
     * @param key 要查询的键
     * @return V 查询到的值拷贝
     * @throw std::out_of_range 如果键不存在
     */
    V get(const K &key) const
    {
        SkipListNode<K, V> *current = head_;
        int curr_lvl = current_level_.load(std::memory_order_acquire);
        for (int i = curr_lvl - 1; i >= 0; i--)
        {
            SkipListNode<K, V> *next_node = current->next[i].load(std::memory_order_acquire);
            while (next_node != nullptr && compare_func_(next_node->key, key) < 0)
            {
                current = next_node;
                next_node = current->next[i].load(std::memory_order_acquire);
            }
        }
        current = current->next[0].load(std::memory_order_acquire);

        if (current != nullptr && compare_func_(current->key, key) == 0)
        {
            return current->get_value(); // 使用 SeqLock 解码
        }
        throw std::out_of_range("Key not found");
    }
    /**
     * @brief 模板化的无锁获取方法，支持兼容不同类型的查询键（例如字符串视图）。
     * @param key 要查询的键
     * @return V 查询到的值拷贝
     * @throw std::out_of_range 如果键不存在
     */
    template <typename SearchKey>
    V get(const SearchKey &key) const
    {
        // 编译期断言：必须能比较 == 和 <
        static_assert(
            // 检查两个类型是否支持相等、小于比较
            std::is_convertible_v<decltype(std::declval<K>() == std::declval<SearchKey>()), bool> &&
                std::is_convertible_v<decltype(std::declval<K>() < std::declval<SearchKey>()), bool>,
            // 自定义清晰报错！
            "Key type must support == and < operators");
        SkipListNode<K, V> *current = head_;
        int curr_lvl = current_level_.load(std::memory_order_acquire);
        for (int i = curr_lvl - 1; i >= 0; i--)
        {
            SkipListNode<K, V> *next_node = current->next[i].load(std::memory_order_acquire);
            while (next_node != nullptr && next_node->key < key)
            {
                current = next_node;
                next_node = current->next[i].load(std::memory_order_acquire);
            }
        }
        current = current->next[0].load(std::memory_order_acquire);

        if (current != nullptr && current->key == key)
        {
            return current->get_value(); // 使用 SeqLock 解码
        }
        throw std::out_of_range("Key not found");
    }

    /**
     * @brief 针对已有节点提供定制化修改（例如逻辑标记删除 Tombstone）。
     *
     * 修改过程受 SeqLock 保护，不会影响并发进行的无锁读操作（撕裂读）。
     *
     * @param key 要修改的键
     * @param value_handle 回调函数，接收旧 Value 引用，返回新 Value 引用
     * @return V 修改前的旧 Value 拷贝
     * @throw std::out_of_range 如果键不存在
     */
    V handle_value(const K &key, std::function<V &(V &)> value_handle)
    {
        SkipListNode<K, V> *current = head_;
        int curr_lvl = current_level_.load(std::memory_order_acquire);
        for (int i = curr_lvl - 1; i >= 0; i--)
        {
            SkipListNode<K, V> *next_node = current->next[i].load(std::memory_order_acquire);
            while (next_node != nullptr && compare_func_(next_node->key, key) < 0)
            {
                current = next_node;
                next_node = current->next[i].load(std::memory_order_acquire);
            }
        }
        current = current->next[0].load(std::memory_order_acquire);

        if (current != nullptr && compare_func_(current->key, key) == 0)
        {
            // 利用 SeqLock 独占 Value 更新周期
            current->seq_.fetch_add(1, std::memory_order_acquire);
            V old_value = current->value;
            size_t old_value_size = calculate_value_size_func_(old_value);

            // 执行业务侧传入的修改逻辑（如修改 tombstone = true）
            current->value = value_handle(current->value);

            size_t new_value_size = calculate_value_size_func_(current->value);
            current->seq_.fetch_add(1, std::memory_order_release); // 释放 SeqLock

            current_size_.fetch_add(new_value_size - old_value_size, std::memory_order_relaxed);
            return old_value;
        }
        throw std::out_of_range("Key not found");
    }

    // =========================================================================
    // 危险操作区 (Physical Deletion)
    // 下列 remove 系列方法执行 **物理删除并释放内存 (delete node)**。
    // 【警告】：不允许在无锁读并发场景下使用！通常供 ZSET 使用（前提是 ZSET 外层包裹了互斥锁）。
    // =========================================================================

    /**
     * @brief 物理删除指定键的节点并释放内存。
     * @warning **必须在无并发读的环境下调用（例如外部互斥锁保护）**。
     *
     * @param key 要删除的键
     * @return true 删除成功
     * @return false 键不存在
     */
    bool remove(const K &key)
    {
        SkipListNode<K, V> *current = head_;
        std::vector<SkipListNode<K, V> *> update(MAX_LEVEL, nullptr);
        int curr_lvl = current_level_.load(std::memory_order_acquire);

        // 查找待删除节点及各层前驱
        for (int i = curr_lvl - 1; i >= 0; i--)
        {
            SkipListNode<K, V> *next_node = current->next[i].load(std::memory_order_acquire);
            while (next_node != nullptr && compare_func_(next_node->key, key) < 0)
            {
                current = next_node;
                next_node = current->next[i].load(std::memory_order_acquire);
            }
            update[i] = current;
        }

        current = current->next[0].load(std::memory_order_acquire);

        if (current != nullptr && compare_func_(current->key, key) == 0)
        {
            // 更新各层前驱指针以解链
            for (int i = 0; i < curr_lvl; i++)
            {
                if (update[i]->next[i].load(std::memory_order_acquire) != current)
                    break;
                update[i]->next[i].store(current->next[i].load(std::memory_order_relaxed), std::memory_order_release);
            }

            // 计算释放的内存量
            size_t key_size = calculate_key_size_func_(current->key);
            size_t value_size = calculate_value_size_func_(current->get_value());
            current_size_.fetch_sub(key_size + value_size + sizeof(std::atomic<SkipListNode<K, V> *>) * current->level + sizeof(SkipListNode<K, V>), std::memory_order_relaxed);

            // 【关键物理删除】
            delete current;
            size_.fetch_sub(1, std::memory_order_relaxed);

            // 如果高层变空，则降低当前跳表最大层级
            int new_level = curr_lvl;
            while (new_level > 1 && head_->next[new_level - 1].load(std::memory_order_acquire) == nullptr)
            {
                new_level--;
            }
            current_level_.store(new_level, std::memory_order_release);
            return true;
        }
        return false;
    }

    /**
     * @brief 物理删除指定键区间 [min_key, max_key] 内的所有节点。
     * @warning 同样受限于物理删除的约束，必须在互斥锁保护下调用。
     *
     * @param min_key 起始键（包含）
     * @param max_key 结束键（包含）
     * @return size_t 成功删除的节点数量
     */
    size_t remove_range_by_key(const K &min_key, const K &max_key)
    {
        if (min_key > max_key)
            return 0;

        size_t removed_count = 0;
        SkipListNode<K, V> *current = head_;
        std::vector<SkipListNode<K, V> *> update(MAX_LEVEL, nullptr);
        int curr_lvl = current_level_.load(std::memory_order_acquire);

        // 查找起始位置及前驱
        for (int i = curr_lvl - 1; i >= 0; i--)
        {
            SkipListNode<K, V> *next_node = current->next[i].load(std::memory_order_acquire);
            while (next_node != nullptr && compare_func_(next_node->key, min_key) < 0)
            {
                current = next_node;
                next_node = current->next[i].load(std::memory_order_acquire);
            }
            update[i] = current;
        }

        current = current->next[0].load(std::memory_order_acquire);
        size_t sub_size = 0;

        // 依次遍历并删除范围内的节点
        while (current != nullptr && compare_func_(current->key, max_key) <= 0)
        {
            SkipListNode<K, V> *to_delete = current;
            current = current->next[0].load(std::memory_order_acquire);

            for (int i = 0; i < curr_lvl; i++)
            {
                if (update[i]->next[i].load(std::memory_order_acquire) != to_delete)
                    break;
                update[i]->next[i].store(to_delete->next[i].load(std::memory_order_relaxed), std::memory_order_release);
            }

            size_t key_size = calculate_key_size_func_(to_delete->key);
            size_t value_size = calculate_value_size_func_(to_delete->get_value());
            sub_size += key_size + value_size + sizeof(std::atomic<SkipListNode<K, V> *>) * to_delete->level + sizeof(SkipListNode<K, V>);

            delete to_delete;
            removed_count++;
            size_.fetch_sub(1, std::memory_order_relaxed);
        }

        current_size_.fetch_sub(sub_size, std::memory_order_relaxed);

        int new_level = curr_lvl;
        while (new_level > 1 && head_->next[new_level - 1].load(std::memory_order_acquire) == nullptr)
        {
            new_level--;
        }
        current_level_.store(new_level, std::memory_order_release);

        return removed_count;
    }

    /**
     * @brief 物理删除指定排名区间内的所有节点。
     * @warning 必须在互斥锁保护下调用。
     *
     * @param start_rank 起始排名（基于 0，包含）
     * @param end_rank 结束排名（包含）
     * @return size_t 成功删除的节点数量
     */
    size_t remove_range_by_rank(size_t start_rank, size_t end_rank)
    {
        if (start_rank > end_rank)
            return 0;

        K min_key, max_key;
        SkipListNode<K, V> *current = head_->next[0].load(std::memory_order_acquire);
        size_t index = 0;

        while (current != nullptr)
        {
            if (index == start_rank)
                min_key = current->key;
            if (index == end_rank)
            {
                max_key = current->key;
                break;
            }
            current = current->next[0].load(std::memory_order_acquire);
            index++;
        }
        return remove_range_by_key(min_key, max_key);
    }

    /**
     * @brief 获取跳表中有效节点总数。
     * @return size_t 节点数
     */
    size_t size() const { return size_.load(std::memory_order_relaxed); }

    /**
     * @brief 获取跳表当前估算的内存使用量。
     * @return size_t 内存占用（字节数）
     */
    size_t memory_usage() const { return current_size_.load(std::memory_order_relaxed); }

    /**
     * @brief 清空整个跳表并恢复到初始化状态。
     * @warning 不受无锁读保护，必须在互斥环境（独占锁）下进行。
     */
    void clear()
    {
        SkipListNode<K, V> *current = head_->next[0].load(std::memory_order_relaxed);
        while (current != nullptr)
        {
            SkipListNode<K, V> *next = current->next[0].load(std::memory_order_relaxed);
            delete current;
            current = next;
        }
        for (int i = 0; i < MAX_LEVEL; ++i)
        {
            head_->next[i].store(nullptr, std::memory_order_relaxed);
        }
        current_level_.store(1, std::memory_order_relaxed);
        size_.store(0, std::memory_order_relaxed);
        init_current_size();
    }

    /**
     * @brief 按照排名范围提取键值对（支持并发读）。
     *
     * @param start_rank 起始排名（基于 0）
     * @param end_rank 结束排名
     * @return std::vector<std::pair<K, V>> 符合排名范围的键值对列表
     */
    std::vector<std::pair<K, V>> range_by_rank(size_t start_rank, size_t end_rank) const
    {
        if (start_rank > end_rank)
            return {};
        std::vector<std::pair<K, V>> result;
        SkipListNode<K, V> *current = head_->next[0].load(std::memory_order_acquire);
        size_t index = 0;
        while (current != nullptr && index <= end_rank)
        {
            if (index >= start_rank)
                result.emplace_back(current->key, current->get_value());
            current = current->next[0].load(std::memory_order_acquire);
            index++;
        }
        return result;
    }

    /**
     * @brief 按照键范围提取键值对（支持并发读）。
     *
     * @param min_key 起始键
     * @param max_key 结束键
     * @return std::vector<std::pair<K, V>> 符合键范围的键值对列表
     */
    std::vector<std::pair<K, V>> range_by_key(const K &min_key, const K &max_key) const
    {
        if (min_key > max_key)
            return {};
        std::vector<std::pair<K, V>> result;
        SkipListNode<K, V> *current = head_;
        int curr_lvl = current_level_.load(std::memory_order_acquire);
        for (int i = curr_lvl - 1; i >= 0; i--)
        {
            SkipListNode<K, V> *next_node = current->next[i].load(std::memory_order_acquire);
            while (next_node != nullptr && compare_func_(next_node->key, min_key) <= 0)
            {
                current = next_node;
                next_node = current->next[i].load(std::memory_order_acquire);
            }
        }

        while (current != nullptr && compare_func_(current->key, max_key) <= 0)
        {
            result.emplace_back(current->key, current->get_value());
            current = current->next[0].load(std::memory_order_acquire);
        }
        return result;
    }

    /**
     * @brief 查询指定键在跳表中的正序排名（支持并发读）。
     *
     * @param key 要查询的键
     * @return std::optional<size_t> 排名（基于0），若不存在返回 nullopt
     */
    std::optional<size_t> rank(const K &key) const
    {
        size_t rank = 0;
        SkipListNode<K, V> *current = head_->next[0].load(std::memory_order_acquire);
        while (current != nullptr && compare_func_(current->key, key) < 0)
        {
            rank++;
            current = current->next[0].load(std::memory_order_acquire);
        }
        if (current != nullptr && compare_func_(current->key, key) == 0)
        {
            return rank;
        }
        return std::nullopt;
    }

    /**
     * @brief 获取跳表内包含的所有键值对（支持并发读）。
     * 一般用于 Memtable 的 Flush 操作。
     *
     * @return std::vector<std::pair<K, V>> 升序排列的键值对列表
     */
    std::vector<std::pair<K, V>> get_all_entries() const
    {
        std::vector<std::pair<K, V>> result;
        SkipListNode<K, V> *current = head_->next[0].load(std::memory_order_acquire);
        while (current != nullptr)
        {
            result.emplace_back(current->key, current->get_value());
            current = current->next[0].load(std::memory_order_acquire);
        }
        return result;
    }

    /**
     * @brief 遍历整个跳表执行回调（支持并发读）。
     *
     * @param callback 回调函数，接受每个有效节点的键和值拷贝
     */
    void for_each(std::function<void(const K &key, const V &value)> callback) const
    {
        SkipListNode<K, V> *current = head_->next[0].load(std::memory_order_acquire);
        while (current != nullptr)
        {
            callback(current->key, current->get_value());
            current = current->next[0].load(std::memory_order_acquire);
        }
    }

    /**
     * @brief 将跳表序列化为二进制字符串。
     * @note 序列化应当在互斥/独占状态下进行，防止序列化途中发生结构变更。
     *
     * @param serialize_key_func 序列化 K 的外部函数
     * @param serialize_value_func 序列化 V 的外部函数
     * @return std::string 序列化后的二进制流
     */
    std::string serialize(std::string (*serialize_key_func)(const K &key), std::string (*serialize_value_func)(const V &value)) const
    {
        std::string result;
        int curr_lvl = current_level_.load(std::memory_order_acquire);
        size_t sz = size_.load(std::memory_order_acquire);

        // 使用网络字节序保证跨平台兼容性
        int c_lvl_net = htonl(curr_lvl);
        uint32_t size_net = htonl(static_cast<uint32_t>(sz));

        result.append(reinterpret_cast<const char *>(&c_lvl_net), sizeof(c_lvl_net));
        result.append(reinterpret_cast<const char *>(&size_net), sizeof(size_net));

        SkipListNode<K, V> *current = head_->next[0].load(std::memory_order_acquire);
        while (current != nullptr)
        {
            uint32_t next_size = htonl(static_cast<uint32_t>(current->level));
            result.append(reinterpret_cast<const char *>(&next_size), sizeof(next_size));
            result.append(serialize_key_func(current->key));
            result.append(serialize_value_func(current->get_value()));
            current = current->next[0].load(std::memory_order_acquire);
        }
        return result;
    }

    /**
     * @brief 从二进制字符串中反序列化并重建跳表。
     * @warning 调用此函数会直接覆盖已有数据，需在独占环境下调用。
     *
     * @param data 二进制数据指针
     * @param offset 初始读取偏移量（反序列化后将更新为新偏移量）
     * @param deserialize_key_func 反序列化 K 的外部函数
     * @param deserialize_value_func 反序列化 V 的外部函数
     */
    void deserialize(const char *data, size_t &offset,
                     K (*deserialize_key_func)(const char *data, size_t &offset),
                     V (*deserialize_value_func)(const char *data, size_t &offset))
    {
        int c_lvl_net;
        std::memcpy(&c_lvl_net, data + offset, sizeof(c_lvl_net));
        int curr_lvl = ntohl(c_lvl_net);
        if (curr_lvl > MAX_LEVEL)
            curr_lvl = MAX_LEVEL;
        current_level_.store(curr_lvl, std::memory_order_release);
        offset += sizeof(c_lvl_net);

        uint32_t size_net;
        std::memcpy(&size_net, data + offset, sizeof(size_net));
        offset += sizeof(size_net);
        size_t sz = static_cast<size_t>(ntohl(size_net));

        std::vector<SkipListNode<K, V> *> level_nodes(curr_lvl + 1, head_);

        for (size_t i = 0; i < sz; ++i)
        {
            uint32_t next_size;
            std::memcpy(&next_size, data + offset, sizeof(next_size));
            offset += sizeof(next_size);
            next_size = ntohl(next_size);

            K key = deserialize_key_func(data, offset);
            V value = deserialize_value_func(data, offset);

            SkipListNode<K, V> *node = new SkipListNode<K, V>(key, value, next_size);

            // 逐层建立 next 链接
            for (size_t j = 0; j < next_size; j++)
            {
                level_nodes[j]->next[j].store(node, std::memory_order_relaxed);
                level_nodes[j] = node;
            }
        }
        size_.store(sz, std::memory_order_release);
    }
};
#endif