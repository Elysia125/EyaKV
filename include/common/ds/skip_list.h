#ifndef SKIP_LIST_H
#define SKIP_LIST_H

#include <iostream>
#include <string>
#include <vector>
#include <shared_mutex>
#include <random>
#include <ctime>
#include <cstring>
#include <atomic>
#include <optional>
#include <functional>
#include <mutex>
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif
#define DEFAULT_MAX_LEVEL 16
#define DEFAULT_PROBABILITY 0.5

template <typename K, typename V>
struct SkipListNode
{
    /**
     * @brief The key of the node
     */
    K key;
    /**
     * @brief The value stored in the node
     */
    V value;
    /**
     * @brief Vector of pointers to the next nodes at each level
     */
    std::vector<SkipListNode<K, V> *> next;

    /**
     * @brief Constructs a SkipListNode with the specified key, value, and level
     * @param key The key to store
     * @param value The value to store
     * @param level The number of levels this node should be present in
     */
    SkipListNode(const K &key, const V &value, int level) : key(key), value(value)
    {
        next.resize(level, nullptr);
    }
};

template <typename K, typename V>
class SkipList
{
private:
    /**
     * @brief 跳表的最大层数
     */
    const size_t MAX_LEVEL;
    /**
     * @brief 节点层数的概率因子
     */
    const double PROBABILITY;
    /**
     * @brief 当前跳表的最大层数
     */
    int current_level_;
    /**
     * @brief 头节点（哨兵节点，不存储实际数据）
     */
    SkipListNode<K, V> *head_;

    /**
     * @brief 互斥锁，保证并发安全
     */
    mutable std::shared_mutex mutex_;
    /**
     * @brief 当前跳表的元素数量
     */
    size_t size_ = 0;
    /**
     * @brief Key比较函数指针
     * @param a 第一个key
     * @param b 第二个key
     * @return -1表示a<b，0表示a==b，1表示a>b
     */
    int (*compare_func_)(const K &, const K &);
    /**
     * @brief 当前使用的内存大小估算（原子变量）
     */
    std::atomic<size_t> current_size_{0};
    /**
     * @brief 计算key的内存大小的函数指针
     */
    size_t (*calculate_key_size_func_)(const K &);
    /**
     * @brief 计算value的内存大小的函数指针
     */
    size_t (*calculate_value_size_func_)(const V &);

    /**
     * @brief 核心辅助函数：随机生成新节点的层数（基于概率算法）
     * @return 生成的层数（1到MAX_LEVEL之间）
     * @note 每一层有50%的概率向上增加，直到达到最大层数
     */
    int random_level()
    {
        int level = 1;
        // 50%概率向上一层，直到达到最大层数

        while (generate_random_01() < PROBABILITY && level < MAX_LEVEL)
        {
            level++;
        }
        return level;
    }

    /**
     * @brief 生成 [0.0, 1.0) 区间的均匀随机数
     * @return [0.0, 1.0) 范围内的随机浮点数
     * @note 特点：线程安全、分布均匀、种子唯一（避免重复序列）
     */
    double generate_random_01()
    {
        // 1. 静态随机数引擎（仅初始化一次，避免重复生成相同序列）
        // mt19937：梅森旋转算法，周期长（2^19937-1）、效率高
        static std::mt19937 engine(
            // 种子初始化：优先用硬件随机数生成器， fallback 到高精度时间
            []() -> uint32_t
            {
                std::random_device rd; // 硬件随机数（尽可能获取真随机）
                if (rd.entropy() > 0)
                { // 检查是否支持硬件随机数
                    return rd();
                }
                else
                {
                    // 无硬件随机数时，用高精度时间作为种子（比 time(0) 精度高）
                    auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
                    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
                }
            }());

        // 2. 均匀分布器：指定 [0.0, 1.0) 区间（左闭右开）
        static std::uniform_real_distribution<double> dist(0.0, 1.0);

        // 3. 生成随机数
        return dist(engine);
    }

    /**
     * @brief 初始化当前内存大小估算值
     * @note 计算头节点占用的内存大小
     */
    void init_current_size()
    {
        current_size_.store(calculate_key_size_func_(K()) + calculate_value_size_func_(V()) + sizeof(SkipListNode<K, V> *) * MAX_LEVEL, std::memory_order_relaxed);
    }

public:
    /**
     * @brief Constructs a SkipList with customizable parameters
     * @param skiplist_max_level The maximum number of levels in the skip list (default: 16)
     * @param skiplist_probability The probability factor for determining node level (default: 0.5)
     * @param compare_func Optional custom comparison function for keys
     * @param calculate_key_size_func Optional custom function to calculate key memory size
     * @param calculate_value_size_func Optional custom function to calculate value memory size
     */
    SkipList(const size_t &skiplist_max_level = DEFAULT_MAX_LEVEL,
             const double &skiplist_probability = DEFAULT_PROBABILITY,
             std::optional<int (*)(const K &, const K &)> compare_func = std::nullopt,
             std::optional<size_t (*)(const K &)> calculate_key_size_func = std::nullopt,
             std::optional<size_t (*)(const V &)> calculate_value_size_func = std::nullopt) : current_level_(1),
                                                                                              size_(0),
                                                                                              current_size_(0),
                                                                                              MAX_LEVEL(skiplist_max_level),
                                                                                              PROBABILITY(skiplist_probability)
    {
        if (compare_func.has_value())
        {
            compare_func_ = compare_func.value();
        }
        else
        {
            compare_func_ = [](const K &a, const K &b)
            {
                return (a < b) ? -1 : ((a > b) ? 1 : 0);
            };
        }
        if (calculate_key_size_func.has_value())
        {
            calculate_key_size_func_ = calculate_key_size_func.value();
        }
        else
        {
            calculate_key_size_func_ = [](const K &key)
            {
                return sizeof(key);
            };
        }
        if (calculate_value_size_func.has_value())
        {
            calculate_value_size_func_ = calculate_value_size_func.value();
        }
        else
        {
            calculate_value_size_func_ = [](const V &value)
            {
                return sizeof(value);
            };
        }
        //  头节点键值无意义，层数为最大层数
        head_ = new SkipListNode<K, V>(K(), V(), MAX_LEVEL);
        init_current_size();
    }

    /**
     * @brief Destroys the SkipList and releases all node memory
     */
    ~SkipList()
    {
        SkipListNode<K, V> *current = head_;
        while (current != nullptr)
        {
            SkipListNode<K, V> *next = current->next[0];
            delete current;
            current = next;
        }
    }

    /**
     * @brief Copy constructor
     * @param sl The SkipList to copy from
     * @note This constructor actually moves resources instead of copying
     */
    SkipList(const SkipList &other)
        : current_level_(1),
          MAX_LEVEL(other.MAX_LEVEL),
          PROBABILITY(other.PROBABILITY),
          compare_func_(other.compare_func_),
          calculate_key_size_func_(other.calculate_key_size_func_),
          calculate_value_size_func_(other.calculate_value_size_func_)
    {
        head_ = new SkipListNode<K, V>(K(), V(), MAX_LEVEL);
        size_ = 0;
        init_current_size();

        SkipListNode<K, V> *current = other.head_->next[0];
        while (current != nullptr)
        {
            insert(current->key, current->value);
            current = current->next[0];
        }
    }

    SkipList &operator=(const SkipList &other)
    {
        if (&other == this)
        {
            return *this;
        }

        clear();

        compare_func_ = other.compare_func_;
        calculate_key_size_func_ = other.calculate_key_size_func_;
        calculate_value_size_func_ = other.calculate_value_size_func_;

        SkipListNode<K, V> *current = other.head_->next[0];
        while (current != nullptr)
        {
            insert(current->key, current->value);
            current = current->next[0];
        }

        return *this;
    }

    SkipList(SkipList &&other) noexcept
        : current_level_(other.current_level_),
          head_(other.head_),
          size_(other.size_),
          current_size_(other.current_size_.load(std::memory_order_relaxed)),
          MAX_LEVEL(other.MAX_LEVEL),
          PROBABILITY(other.PROBABILITY),
          compare_func_(other.compare_func_),
          calculate_key_size_func_(other.calculate_key_size_func_),
          calculate_value_size_func_(other.calculate_value_size_func_)
    {
        // 将原对象置为空状态
        other.head_ = nullptr;
        other.size_ = 0;
        other.current_level_ = 1;
    }
    SkipList &operator=(SkipList &&other) noexcept
    {
        if (&other == this)
        {
            return *this;
        }

        // 清理当前资源
        SkipListNode<K, V> *current = head_;
        while (current != nullptr)
        {
            SkipListNode<K, V> *next = current->next[0];
            delete current;
            current = next;
        }

        // 移动资源
        current_level_ = other.current_level_;
        head_ = other.head_;
        current_size_.store(other.current_size_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        size_ = other.size_;
        compare_func_ = other.compare_func_;
        calculate_key_size_func_ = other.calculate_key_size_func_;
        calculate_value_size_func_ = other.calculate_value_size_func_;

        // 将原对象置为空状态
        other.head_ = nullptr;
        other.size_ = 0;
        other.current_level_ = 1;

        return *this;
    }

    /**
     * @brief 向跳表中插入或更新一个键值对
     * @param key 要插入的key
     * @param value 要插入的value
     * @throw std::overflow_error 当达到最大节点数时抛出异常
     * @note 如果key已存在，则更新value；否则插入新节点
     */
    void insert(const K &key, const V &value)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        size_t new_key_size = calculate_key_size_func_(key);
        size_t new_value_size = calculate_value_size_func_(value);

        SkipListNode<K, V> *current = head_;
        // 记录各层的前驱节点
        std::vector<SkipListNode<K, V> *> update(MAX_LEVEL, nullptr);
        // 查找插入位置
        for (int i = current_level_ - 1; i >= 0; i--)
        {
            while (current->next[i] != nullptr && compare_func_(current->next[i]->key, key) < 0)
            {
                current = current->next[i];
            }
            update[i] = current;
        }
        current = current->next[0];
        // 如果key已经存在，则更新value
        if (current != nullptr && compare_func_(current->key, key) == 0)
        {
            size_t old_value_size = calculate_value_size_func_(current->value);
            current->value = value;
            current_size_.fetch_add(new_value_size - old_value_size, std::memory_order_relaxed);
            return;
        }
        // 生成新节点
        int level = random_level();
        if (level > current_level_)
        {
            for (int i = current_level_; i < level; i++)
            {
                update[i] = head_;
            }
            current_level_ = level;
        }
        SkipListNode<K, V> *new_node = new SkipListNode<K, V>(key, value, level);
        // 插入新节点
        try
        {
            for (int i = 0; i < level; i++)
            {
                new_node->next[i] = update[i]->next[i];
                update[i]->next[i] = new_node;
            }
            size_++;
            current_size_.fetch_add(new_key_size + new_value_size + sizeof(SkipListNode<K, V> *) * level + sizeof(SkipListNode<K, V>), std::memory_order_relaxed);
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
        }
    }

    /**
     * @brief 根据key获取对应的value
     * @param key 要查找的key
     * @return 对应的value
     * @throw std::out_of_range 当key不存在或已过期/被标记删除时抛出异常
     * @note 如果key已过期，会自动标记为删除状态
     */
    V get(const K &key) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        SkipListNode<K, V> *current = head_;
        for (int i = current_level_ - 1; i >= 0; i--)
        {
            while (current->next[i] != nullptr && compare_func_(current->next[i]->key, key) < 0)
            {
                current = current->next[i];
            }
        }
        current = current->next[0];
        if (current != nullptr && compare_func_(current->key, key) == 0)
        {
            return current->value;
        }
        throw std::out_of_range("Key not found");
    }

    /**
     * @brief 对某个key对应的value进行操作
     * @param key 要操作的key
     * @param value_handle 操作函数，接受一个V&参数，返回一个V
     * @return 操作之前的value
     * @throw std::out_of_range 当key不存在或已过期/被标记删除时抛出异常
     * @note 如果key已过期，会自动标记为删除状态
     */
    V handle_value(const K &key, std::function<V &(V &)> value_handle)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        SkipListNode<K, V> *current = head_;
        for (int i = current_level_ - 1; i >= 0; i--)
        {
            while (current->next[i] != nullptr && compare_func_(current->next[i]->key, key) < 0)
            {
                current = current->next[i];
            }
        }
        current = current->next[0];
        if (current != nullptr && compare_func_(current->key, key) == 0)
        {
            V old_value = current->value;
            size_t old_value_size = calculate_value_size_func_(old_value);
            current->value = value_handle(current->value);
            size_t new_value_size = calculate_value_size_func_(current->value);
            current_size_.fetch_add(new_value_size - old_value_size, std::memory_order_relaxed);
            return old_value;
        }
        throw std::out_of_range("Key not found");
    }
    /**
     * @brief 从跳表中移除指定key的节点（物理删除）
     * @param key 要移除的key
     * @return true表示成功移除，false表示key不存在
     * @note 此操作会实际删除节点并释放内存
     */
    bool remove(const K &key)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        SkipListNode<K, V> *current = head_;
        std::vector<SkipListNode<K, V> *> update(MAX_LEVEL, nullptr);
        for (int i = current_level_ - 1; i >= 0; i--)
        {
            while (current->next[i] != nullptr && compare_func_(current->next[i]->key, key) < 0)
            {
                current = current->next[i];
            }
            update[i] = current;
        }
        current = current->next[0];
        if (current != nullptr && compare_func_(current->key, key) != 0)
        {
            return false;
        }
        for (int i = 0; i < current_level_; i++)
        {
            if (update[i]->next[i] != current)
            {
                break;
            }
            update[i]->next[i] = current->next[i];
        }
        size_t key_size = calculate_key_size_func_(current->key);
        size_t value_size = calculate_value_size_func_(current->value);
        current_size_.fetch_sub(key_size + value_size + sizeof(SkipListNode<K, V> *) * current->next.size() + sizeof(SkipListNode<K, V>), std::memory_order_relaxed);

        delete current;
        size_--;
        while (current_level_ > 1 && head_->next[current_level_ - 1] == nullptr)
        {
            current_level_--;
        }
        return true;
    }

    /**
     * @brief 获取跳表中的元素数量
     * @return 跳表中当前存储的元素数量
     * @note 不包括被标记删除但未物理删除的节点
     */
    size_t size() const
    {
        return size_;
    }

    /**
     * @brief 获取跳表的内存使用量估算
     * @return 跳表当前使用的内存大小（字节）
     */
    size_t memory_usage() const
    {
        return current_size_.load(std::memory_order_relaxed);
    }

    /**
     * @brief 清空跳表中的所有节点
     * @note 释放所有节点的内存并重置跳表状态
     */
    void clear()
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        SkipListNode<K, V> *current = head_;
        while (current != nullptr)
        {
            SkipListNode<K, V> *next = current->next[0];
            delete current;
            current = next;
        }
        head_ = new SkipListNode<K, V>(K(), V(), MAX_LEVEL);
        current_level_ = 1;
        size_ = 0;
        init_current_size();
    }

    /**
     * @brief 按照排名范围获取键值对
     * @param start_rank 起始排名（从0开始）
     * @param end_rank 结束排名
     * @return 指定排名范围内的键值对列表
     * @note 会跳过已过期或被标记删除的节点
     */
    std::vector<std::pair<K, V>> range_by_rank(size_t start_rank, size_t end_rank) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (start_rank > end_rank)
        {
            return {};
        }
        std::vector<std::pair<K, V>> result;
        SkipListNode<K, V> *current = head_->next[0];
        size_t index = 0;
        while (current != nullptr && index <= end_rank)
        {
            if (index >= start_rank)
            {
                result.emplace_back(current->key, current->value);
            }
            current = current->next[0];
            index++;
        }
        return result;
    }

    /**
     * @brief 按照key范围获取键值对
     * @param min_key 起始key（包含）
     * @param max_key 结束key（包含）
     * @return 指定key范围内的键值对列表
     * @note 会跳过已过期或被标记删除的节点
     */
    std::vector<std::pair<K, V>> range_by_key(const K &min_key, const K &max_key) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (min_key > max_key)
        {
            return {};
        }
        std::vector<std::pair<K, V>> result;
        SkipListNode<K, V> *current = head_;
        // 查找起始位置
        for (int i = current_level_ - 1; i >= 0; i--)
        {
            while (current->next[i] != nullptr && compare_func_(current->next[i]->key, min_key) <= 0)
            {
                current = current->next[i];
            }
        }
        while (current != nullptr && compare_func_(current->key, max_key) <= 0)
        {
            result.emplace_back(current->key, current->value);
            current = current->next[0];
        }
        return result;
    }

    /**
     * @brief 物理删除指定key范围内的所有节点
     * @param min_key 起始key
     * @param max_key 结束key
     * @return 被删除的节点数量
     * @note 此操作会实际删除节点并释放内存
     */
    size_t remove_range_by_key(const K &min_key, const K &max_key)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (min_key > max_key)
        {
            return 0;
        }
        size_t removed_count = 0;
        SkipListNode<K, V> *current = head_;
        std::vector<SkipListNode<K, V> *> update(MAX_LEVEL, nullptr);
        // 查找起始位置
        for (int i = current_level_ - 1; i >= 0; i--)
        {
            while (current->next[i] != nullptr && compare_func_(current->next[i]->key, min_key) < 0)
            {
                current = current->next[i];
            }
            update[i] = current;
        }
        current = current->next[0];
        size_t sub_size = 0;
        // 删除范围内的节点
        while (current != nullptr && compare_func_(current->key, max_key) <= 0)
        {
            SkipListNode<K, V> *to_delete = current;
            current = current->next[0];
            for (int i = 0; i < current_level_; i++)
            {
                if (update[i]->next[i] != to_delete)
                {
                    break;
                }
                update[i]->next[i] = to_delete->next[i];
            }
            size_t key_size = calculate_key_size_func_(to_delete->key);
            size_t value_size = calculate_value_size_func_(to_delete->value);
            sub_size += key_size + value_size + sizeof(SkipListNode<K, V> *) * to_delete->next.size() + sizeof(SkipListNode<K, V>);
            delete to_delete;
            removed_count++;
            size_--;
        }
        current_size_.fetch_sub(sub_size, std::memory_order_relaxed);
        // 调整当前层数
        while (current_level_ > 1 && head_->next[current_level_ - 1] == nullptr)
        {
            current_level_--;
        }
        return removed_count;
    }

    /**
     * @brief 物理删除指定排名范围内的所有节点
     * @param start_rank 起始排名
     * @param end_rank 结束排名
     * @return 被删除的节点数量
     * @note 内部通过key范围删除实现
     */
    size_t remove_range_by_rank(size_t start_rank, size_t end_rank)
    {
        if (start_rank > end_rank)
        {
            return 0;
        }
        // 找到开始的key和结束的key
        K min_key, max_key;
        SkipListNode<K, V> *current = head_->next[0];
        size_t index = 0;
        while (current != nullptr)
        {
            if (index == start_rank)
            {
                min_key = current->key;
            }
            if (index == end_rank)
            {
                max_key = current->key;
                break;
            }
            current = current->next[0];
            index++;
        }
        return remove_range_by_key(min_key, max_key);
    }

    /**
     * @brief 获取指定key的排名
     * @param key 要查询的key
     * @return key的排名（如果存在），否则返回std::nullopt
     * @note 排名从0开始计数
     */
    std::optional<size_t> rank(const K &key) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        size_t rank = 0;
        SkipListNode<K, V> *current = head_->next[0];
        while (current != nullptr && compare_func_(current->key, key) < 0)
        {
            rank++;
            current = current->next[0];
        }
        if (current != nullptr && compare_func_(current->key, key) == 0)
        {
            return rank;
        }
        return std::nullopt;
    }

    /**
     * @brief 遍历跳表中的所有有效节点
     * @param callback 对每个有效节点执行的回调函数
     * @note 会跳过已过期或被标记删除的节点
     */
    void for_each(std::function<void(const K &key, const V &value)> callback) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        SkipListNode<K, V> *current = head_->next[0];
        while (current != nullptr)
        {
            callback(current->key, current->value);
            current = current->next[0];
        }
    }

    /**
     * @brief 获取跳表中所有有效的键值对
     * @return 包含所有有效键值对的列表
     * @note 会跳过已过期或被标记删除的节点
     */
    std::vector<std::pair<K, V>> get_all_entries() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<std::pair<K, V>> result;
        SkipListNode<K, V> *current = head_->next[0];
        while (current != nullptr)
        {
            result.emplace_back(current->key, current->value);
            current = current->next[0];
        }
        return result;
    }

    /**
     * @brief 将跳表序列化为字符串
     * @param serialize_key_func 序列化key的函数指针
     * @param serialize_value_func 序列化value的函数指针
     * @return 序列化后的字符串
     * @note 序列化格式：current_level + size + [node_level + key + value + deleted + expire_time]*
     */
    std::string serialize(std::string (*serialize_key_func)(const K &key), std::string (*serialize_value_func)(const V &value)) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::string result;
        int current_level = htonl(current_level_);
        uint32_t size = htonl(static_cast<uint32_t>(size_));
        result.append(reinterpret_cast<const char *>(current_level), sizeof(current_level));
        result.append(reinterpret_cast<const char *>(size), sizeof(size));
        SkipListNode<K, V> *current = head_->next[0];
        while (current != nullptr)
        {
            uint32_t next_size = htonl(static_cast<uint32_t>(current->next.size()));
            result.append(reinterpret_cast<const char *>(&next_size), sizeof(next_size));
            result.append(serialize_key_func(current->key));
            result.append(serialize_value_func(current->value));
            current = current->next[0];
        }
        return result;
    }

    /**
     * @brief 从字符串反序列化跳表
     * @param data 序列化数据的指针
     * @param offset 数据的起始偏移量
     * @param deserialize_key_func 反序列化key的函数指针
     * @param deserialize_value_func 反序列化value的函数指针
     * @note 会清空当前跳表并重建所有节点
     */
    void deserialize(const char *data, size_t &offset,
                     std::string (*deserialize_key_func)(const char *data, size_t &offset),
                     std::string (*deserialize_value_func)(const char *data, size_t &offset))
    {
        int current_level;
        std::memcpy(&current_level, data + offset, sizeof(current_level));
        current_level_ = ntohl(current_level);
        if (current_level_ > MAX_LEVEL)
        {
            current_level_ = MAX_LEVEL;
        }
        std::vector<SkipListNode<K, V> *> level_nodes(current_level_ + 1, head_);
        offset += sizeof(current_level);
        uint32_t size;
        std::memcpy(&size, data + offset, sizeof(size));
        offset += sizeof(size);
        size_ = static_cast<size_t>(ntohl(size));
        for (size_t i = 0; i < size_; ++i)
        {
            uint32_t next_size;
            std::memcpy(&next_size, data + offset, sizeof(next_size));
            offset += sizeof(next_size);
            next_size = ntohl(next_size);
            K key = deserialize_key_func(data, offset);
            V value = deserialize_value_func(data, offset);
            SkipListNode<K, V> *node = new SkipListNode<K, V>(key, value, next_size);
            for (size_t j = 0; j < next_size; j++)
            {
                level_nodes[j]->next[j] = node;
                level_nodes[j] = node;
            }
        }
    }

    /**
     * @brief 打印跳表的当前结构（用于调试）
     * @note 按层级输出所有节点，显示key、value、是否过期、是否删除、节点层数等信息
     */
    void print()
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        std::cout << "SkipList current level: " << current_level_ << std::endl;

        for (int i = current_level_ - 1; i >= 0; --i)
        {
            std::cout << "Level " << i + 1 << ": ";
            SkipListNode<K, V> *current = head_->next[i];
            while (current != nullptr)
            {
                std::cout << "(" << current->key << "," << current->value << "," << current->next.size() << ") ";
                current = current->next[i];
            }
            std::cout << std::endl;
        }
    }
};
#endif