#ifndef LRU_CACHE_H_
#define LRU_CACHE_H_

#include <list>
#include <unordered_map>
#include <vector>
#include <functional>
#include <mutex>

template <typename T>
class SerializeCommand
{
public:
    virtual std::string serialize(T &value) = 0;
    virtual T deserialize(const char *data, size_t &offset) = 0;
    virtual ~SerializeCommand() = default;
};

template <typename Key, typename Value>
class LRUCache
{
public:
    using KeyValuePair = std::pair<Key, Value>;
    using ListIterator = typename std::list<KeyValuePair>::iterator;

    explicit LRUCache(size_t capacity) : capacity_(capacity) {}

    // 获取数据
    bool get(const Key &key, Value &value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end())
        {
            return false;
        }
        // 移动到链表头部 (最近使用)
        items_.splice(items_.begin(), items_, it->second);
        value = it->second->second;
        return true;
    }

    // 写入数据
    void put(const Key &key, const Value &value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it != index_.end())
        {
            // 更新值并移动到头部
            it->second->second = value;
            items_.splice(items_.begin(), items_, it->second);
        }
        else
        {
            // 插入新元素
            if (items_.size() >= capacity_)
            {
                // 淘汰最久未使用的 (链表尾部)
                auto last = items_.end();
                last--;
                index_.erase(last->first);
                items_.pop_back();
            }
            items_.push_front({key, value});
            index_[key] = items_.begin();
        }
    }

    // 检查是否存在
    bool exists(const Key &key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return index_.find(key) != index_.end();
    }

    // 清空缓存
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
        index_.clear();
    }
    // 设置value的序列化
    void set_value_serializer(std::shared_ptr<SerializeCommand<Value>> serializer)
    {
        value_serializer_ = serializer;
    }
    // 设置key的序列化
    void set_key_serializer(std::shared_ptr<SerializeCommand<Key>> serializer)
    {
        key_serializer_ = serializer;
    }
    // 序列化
    std::string serialize()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!value_serializer_ || !key_serializer_)
        {
            throw std::runtime_error("value_serializer_ or key_serializer_ is null");
        }
        std::string data;
        // 1. 写入容量 (虽然后续恢复可能由构造函数决定，但记录一下无妨)
        // 2. 写入大小
        uint32_t size = static_cast<uint32_t>(items_.size());
        data.append(reinterpret_cast<const char *>(&size), sizeof(size));

        // 3. 按顺序写入 (从旧到新还是从新到旧都可以，这里按 list 顺序从新到旧)
        for (const auto &pair : items_)
        {
            data.append(key_serializer_->serialize(pair.first));
            data.append(value_serializer_->serialize(pair.second));
        }
        return data;
    }

    // 反序列化
    void deserialize(const char *data, size_t &offset)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
        index_.clear();

        uint32_t size;
        std::memcpy(&size, data + offset, sizeof(size));
        offset += sizeof(size);

        // 注意：因为 put 操作会将元素放到头部，为了保持原有顺序，
        // 我们应该反向插入，或者读取后修正。
        // 简单做法：直接读取并 push_back，最后重建 map
        for (uint32_t i = 0; i < size; ++i)
        {
            Key key = key_serializer_->deserialize(data, offset);
            // 假设 Value (Response) 有静态 deserialize 方法
            Value val = value_serializer_->deserialize(data, offset);
            items_.push_back({key, val});
        }

        // 重建索引
        for (auto it = items_.begin(); it != items_.end(); ++it)
        {
            index_[it->first] = it;
        }
    }

private:
    size_t capacity_;
    std::list<KeyValuePair> items_;
    std::unordered_map<Key, ListIterator> index_;
    std::mutex mutex_;
    std::shared_ptr<SerializeCommand<Value>> value_serializer_ = nullptr;
    std::shared_ptr<SerializeCommand<Key>> key_serializer_ = nullptr;
};

#endif // LRU_CACHE_H_