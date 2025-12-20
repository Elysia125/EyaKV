#include "storage/memtable.h"
#include <gtest/gtest.h>

using namespace tinykv::storage;

TEST(MemTableTest, PutGetDelete) {
    MemTable m;
    m.Put("k1", "v1");
    auto v = m.Get("k1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), "v1");

    m.Delete("k1");
    EXPECT_FALSE(m.Get("k1").has_value());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
