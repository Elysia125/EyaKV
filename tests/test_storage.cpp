#include "storage/storage.h"
#include <gtest/gtest.h>
#include <filesystem>

using namespace tinykv::storage;

class StorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "test_data_storage";
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
    }

    void TearDown() override {
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
    }

    std::string test_dir;
};

TEST_F(StorageTest, BasicPutGetDelete) {
    Storage storage(test_dir);
    
    // Test Put and Get
    EXPECT_TRUE(storage.Put("key1", "value1"));
    auto res1 = storage.Get("key1");
    ASSERT_TRUE(res1.has_value());
    EXPECT_EQ(res1.value(), "value1");

    // Test Overwrite
    EXPECT_TRUE(storage.Put("key1", "value1_updated"));
    auto res2 = storage.Get("key1");
    ASSERT_TRUE(res2.has_value());
    EXPECT_EQ(res2.value(), "value1_updated");

    // Test Delete
    EXPECT_TRUE(storage.Delete("key1"));
    auto res3 = storage.Get("key1");
    EXPECT_FALSE(res3.has_value());
}

TEST_F(StorageTest, Persistence) {
    {
        Storage storage(test_dir);
        EXPECT_TRUE(storage.Put("k1", "v1"));
        EXPECT_TRUE(storage.Put("k2", "v2"));
        EXPECT_TRUE(storage.Delete("k1"));
        // k1 deleted, k2 exists
    }

    // Reopen
    {
        Storage storage(test_dir);
        auto v1 = storage.Get("k1");
        EXPECT_FALSE(v1.has_value()); // Should be deleted

        auto v2 = storage.Get("k2");
        ASSERT_TRUE(v2.has_value());
        EXPECT_EQ(v2.value(), "v2");
    }
}
