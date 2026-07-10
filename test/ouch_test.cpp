#include "generated_ouch_parser.hpp"
#include <gtest/gtest.h>
#include <vector>

using namespace ouch42;

TEST(OuchParserTest, EnterOrder_SizeCheck) {
    EXPECT_EQ(sizeof(EnterOrder), 49);
}

TEST(OuchParserTest, EnterOrder_Parse) {
    std::vector<uint8_t> buffer(49, 0);
    buffer[0] = 'O'; // message_type
    
    // token: "ORDER123456789"
    const char* token = "ORDER123456789";
    std::memcpy(&buffer[1], token, 14);

    buffer[15] = 'B'; // buy_sell

    // shares = 500 (big-endian)
    buffer[19] = 500 & 0xFF;
    buffer[18] = (500 >> 8) & 0xFF;

    // stock = "MSFT    "
    const char* stock = "MSFT    ";
    std::memcpy(&buffer[20], stock, 8);

    // price = 350.0000 (3500000, big-endian)
    uint32_t price = 3500000;
    buffer[31] = price & 0xFF;
    buffer[30] = (price >> 8) & 0xFF;
    buffer[29] = (price >> 16) & 0xFF;
    buffer[28] = (price >> 24) & 0xFF;

    const EnterOrder* order = reinterpret_cast<const EnterOrder*>(buffer.data());

    EXPECT_EQ(order->message_type(), 'O');
    EXPECT_EQ(order->order_token(), "ORDER123456789");
    EXPECT_EQ(order->buy_sell(), 'B');
    EXPECT_EQ(order->shares(), 500);
    EXPECT_EQ(order->stock(), "MSFT    ");
    EXPECT_EQ(order->price(), 3500000);
    EXPECT_EQ(order->price_double(), 350.0000);
}

TEST(OuchParserTest, MessageSizeLookup) {
    EXPECT_EQ(message_size('O'), 49);
    EXPECT_EQ(message_size('X'), 19);
    EXPECT_EQ(message_size('A'), 66);
    EXPECT_EQ(message_size('Z'), 0); // Invalid message
}

TEST(OuchParserTest, MessageNameLookup) {
    EXPECT_STREQ(message_name('O'), "EnterOrder");
    EXPECT_STREQ(message_name('X'), "CancelOrder");
    EXPECT_STREQ(message_name('A'), "OrderAccepted");
    EXPECT_STREQ(message_name('Z'), "Unknown");
}
