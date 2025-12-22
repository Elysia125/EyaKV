#ifndef SKIP_LIST_H
#define SKIP_LIST_H

#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <random>
#include <ctime>

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
    static const int MAX_LEVEL = 16; // 最大层数
    int current_level_;              // 当前跳表的最大层数
    SkipListNode<K, V> *head_;       // 头节点（哨兵节点，不存实际数据）
    std::mutex mutex_;               // 互斥锁（保证并发安全）
    size_t size_ = 0;                // 当前跳表的元素数量
    // 核心辅助函数：随机生成新节点的层数（概率算法）
    int RandomLevel()
    {
        int level = 1;
        // 50%概率向上一层，直到达到最大层数
        while (rand() % 2 == 0 && level < MAX_LEVEL)
        {
            level++;
        }
        return level;
    }

public:
    SkipList() : current_level_(1), size_(0)
    {
        srand(time(nullptr)); // 初始化随机数种子
        // 头节点键值无意义，层数为最大层数
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

    void insert(const K &key, const V &value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SkipListNode<K, V> *current = head_;
        // 记录各层的前驱节点
        SkipListNode<K, V> *update[MAX_LEVEL] = {nullptr};
        // 查找插入位置
        for (int i = current_level_ - 1; i >= 0; i--)
        {
            while (current->next[i] != nullptr && current->next[i]->key < key)
            {
                current = current->next[i];
            }
            update[i] = current;
        }
        current = current->next[0];
        // 如果key已经存在，则更新value
        if (current != nullptr && current->key == key)
        {
            current->value = value;
            return;
        }
        // 生成新节点
        int level = RandomLevel();
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
            while (current->next[i] != nullptr && current->next[i]->key < key)
            {
                current = current->next[i];
            }
        }
        current = current->next[0];
        if (current != nullptr && current->key == key)
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
            while (current->next[i] != nullptr && current->next[i]->key < key)
            {
                current = current->next[i];
            }
            update[i] = current;
        }
        current = current->next[0];
        if (current != nullptr || current->key != key)
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