#ifndef SKIP_LIST_H
#define SKIP_LIST_H

#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <random>
#include <ctime>
#include <cstring>
#define MAX_LEVEL 16
#define PROBABILITY 0.5
#define MAX_NODE_COUNT 1000000

template <typename K, typename V>
struct SkipListNode
{
    K key;
    V value;
    std::vector<SkipListNode<K, V> *> next;
    SkipListNode(const K &key, const V &value, int level) : key(key), value(value)
    {
        next.resize(level, nullptr);
    }
};

template <typename K, typename V>
class SkipList
{
private:
    const size_t MAX_LEVEL;      // 最大层数
    const double PROBABILITY;    // 节点层数的概率
    const size_t MAX_NODE_COUNT; // 最大节点数
    int current_level_;          // 当前跳表的最大层数
    SkipListNode<K, V> *head_;   // 头节点（哨兵节点，不存实际数据）
    std::mutex mutex_;           // 互斥锁（保证并发安全）
    size_t size_ = 0;            // 当前跳表的元素数量
    int (*)(const K &, const K &) compare_func_;
    // 核心辅助函数：随机生成新节点的层数（概率算法）
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
     * 生成 [0.0, 1.0) 区间的均匀随机数
     * 特点：线程安全、分布均匀、种子唯一（避免重复）
     */
    double generate_random_01()
    {
        // 1. 静态随机数引擎（仅初始化一次，避免重复生成相同序列）
        // mt19937：梅森旋转算法，周期长（2^19937-1）、效率高
        static std::mt19937 engine(
            // 种子初始化：优先用硬件随机数生成器， fallback 到高精度时间
            []() -> unsigned int
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

public:
    SkipList(const size_t &skiplist_max_level = MAX_LEVEL,
             const double &skiplist_probability = PROBABILITY,
             const size_t &skiplist_max_node_count = MAX_NODE_COUNT,
             std::optional<int (*)(const K &, const K &)> compare_func = std::nullopt) : current_level_(1), size_(0),
                                                                                         MAX_LEVEL(skiplist_max_level),
                                                                                         PROBABILITY(skiplist_probability),
                                                                                         MAX_NODE_COUNT(skiplist_max_node_count)
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
        // srand(time(nullptr)); // 初始化随机数种子
        //  头节点键值无意义，层数为最大层数
        head_ = new SkipListNode<K, V>(K(), V(), MAX_LEVEL);
    }

    // 析构函数：释放所有节点内存
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

    SkipList(const SkipList &sl) noexcept
        : current_level_(other.current_level_),
          head_(other.head_),
          size_(other.size_),
          MAX_LEVEL(other.MAX_LEVEL),
          PROBABILITY(other.PROBABILITY),
          MAX_NODE_COUNT(other.MAX_NODE_COUNT),
          compare_func_(other.compare_func_)
    {
        // 将原对象置为空状态
        other.head_ = nullptr;
        other.size_ = 0;
        other.current_level_ = 1;
    }
    SkipList &operator=(const SkipList &sl) noexcept
    {
        if (&sl == this)
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
        size_ = other.size_;
        compare_func_ = other.compare_func_;

        // 将原对象置为空状态
        other.head_ = nullptr;
        other.size_ = 0;
        other.current_level_ = 1;

        return *this;
    }

    SkipList(SkipList &&other) noexcept
        : current_level_(other.current_level_),
          head_(other.head_),
          size_(other.size_),
          MAX_LEVEL(other.MAX_LEVEL),
          PROBABILITY(other.PROBABILITY),
          MAX_NODE_COUNT(other.MAX_NODE_COUNT),
          compare_func_(other.compare_func_)
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
        size_ = other.size_;
        compare_func_ = other.compare_func_;

        // 将原对象置为空状态
        other.head_ = nullptr;
        other.size_ = 0;
        other.current_level_ = 1;

        return *this;
    }

    void insert(const K &key, const V &value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SkipListNode<K, V> *current = head_;
        // 记录各层的前驱节点
        SkipListNode<K, V> *update[MAX_LEVEL] = {nullptr};
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
            current->value = value;
            // 返回变化的内存大小
            return;
        }
        if (MAX_NODE_COUNT > 0 && size_ >= MAX_NODE_COUNT)
        {
            throw std::overflow_error("SkipList has reached its maximum node count");
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
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
        }
    }

    V get(const K &key) const
    {
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

    bool remove(const K &key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SkipListNode<K, V> *current = head_;
        SkipListNode<K, V> *update[MAX_LEVEL] = {nullptr};
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
        delete current;
        size_--;
        while (current_level_ > 1 && head_->next[current_level_ - 1] == nullptr)
        {
            current_level_--;
        }
        return true;
    }

    size_t size() const
    {
        return size_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
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
    }

    std::vector<std::pair<K, V>> range_by_rank(size_t start_rank, size_t end_rank) const
    {
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

    std::vector<std::pair<K, V>> range_by_key(const K &min_key, const K &max_key) const
    {
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

    size_t remove_range_by_key(const K &min_key, const K &max_key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (min_key > max_key)
        {
            return 0;
        }
        size_t removed_count = 0;
        SkipListNode<K, V> *current = head_;
        SkipListNode<K, V> *update[MAX_LEVEL] = {nullptr};
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
            delete to_delete;
            removed_count++;
            size_--;
        }
        // 调整当前层数
        while (current_level_ > 1 && head_->next[current_level_ - 1] == nullptr)
        {
            current_level_--;
        }
        return removed_count;
    }

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

    size_t rank(const K &key) const
    {
        size_t rank = 0;
        SkipListNode<K, V> *current = head_->next[0];
        while (current != nullptr && compare_func_(current->key, key) < 0)
        {
            rank++;
            current = current->next[0];
        }
        return rank;
    }

    std::string serialize(std::string (*serialize_key_func)(const K &key), std::string (*serialize_value_func)(const V &value)) const
    {
        std::string result;
        result.append(reinterpret_cast<const char *>(current_level_), sizeof(current_level_));
        result.append(reinterpret_cast<const char *>(size_), sizeof(size_));
        SkipListNode<K, V> *current = head_->next[0];
        while (current != nullptr)
        {
            result.append(reinterpret_cast<const char *>(current->next.size()), sizeof(current->next.size()));
            result.append(serialize_key_func(current->key));
            result.append(serialize_value_func(current->value));
            current = current->next[0];
        }
        return result;
    }

    void deserialize(const char *data, const size_t offset,
                     std::string (*deserialize_key_func)(const char *data, size_t &offset),
                     std::string (*deserialize_value_func)(const char *data, size_t &offset))
    {
        std::memcpy(&current_level_, data + offset, sizeof(current_level));
        if (current_level_ > skiplist_max_level)
        {
            current_level_ = skiplist_max_level;
        }
        SkipListNode<K, V> *level_nodes[current_level_] = {head_};
        offset += sizeof(current_level_);
        std::memcpy(&size_, data + offset, sizeof(size_));
        offset += sizeof(size_);
        for (size_t i = 0; i < size_; ++i)
        {
            size_t next_size;
            std::memcpy(&next_size, data + offset, sizeof(next_size));
            offset += sizeof(next_size);
            K key = deserialize_key_func(data, offset);
            V value = deserialize_value_func(value_data);
            SkipListNode *node = new SkipListNode<K, V>(key, value, next_size);
            for (size_t j = 0; j < next_size; j++)
            {
                level_nodes[j]->next[j] = node;
                level_nodes[j] = node;
            }
        }
    }
    void print()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "SkipList current level: " << current_level_ << std::endl;

        for (int i = current_level_ - 1; i >= 0; --i)
        {
            std::cout << "Level " << i + 1 << ": ";
            SkipListNode<K, V> *current = head_->next[i];
            while (current != nullptr)
            {
                std::cout << "(" << current->key << "," << current->value << ") ";
                current = current->next[i];
            }
            std::cout << std::endl;
        }
    }
};
#endif