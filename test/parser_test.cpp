// =============================================================================
// parser_test.cpp — GTest suite for the ITCH 5.0 Zero-Copy Parser
//
// Covers all 22 message types, grouped by protocol category:
//   - System:       SystemEvent, MWCBDeclineLevel, MWCBStatus
//   - Stock-related: StockDirectory, StockTradingAction, RegSHORestriction,
//                    MarketParticipantPosition, IPOQuotingPeriodUpdate,
//                    LULDAuctionCollar, OperationalHalt
//   - Add-order:    AddOrder, AddOrderMPIDAttribution
//   - Modify-order: OrderExecuted, OrderExecutedWithPrice, OrderCancel,
//                   OrderDelete, OrderReplace
//   - Trade:        TradeNonCross, CrossTrade, BrokenTrade
//   - NOII:         NOII, RetailInterest
//
// Additionally tests dispatch mechanisms (StaticDispatcher, DispatchTable,
// ParseStream) and utility lookups (message_size, message_name).
// =============================================================================
#include "generated_itch_parser.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

using namespace itch50;

// =============================================================================
// Big-endian buffer writers
// =============================================================================

void write_be16(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF;
}
void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
}
void write_be48(uint8_t* p, uint64_t v) {
    p[0] = (v >> 40) & 0xFF; p[1] = (v >> 32) & 0xFF;
    p[2] = (v >> 24) & 0xFF; p[3] = (v >> 16) & 0xFF;
    p[4] = (v >> 8) & 0xFF; p[5] = v & 0xFF;
}
void write_be64(uint8_t* p, uint64_t v) {
    for (int i=0; i<8; ++i) p[i] = (v >> (56 - (i*8))) & 0xFF;
}

// =============================================================================
// SYSTEM MESSAGES
// =============================================================================

// ── SystemEvent ('S', 12B) ──────────────────────────────────────────────────

TEST(SystemMessages, SystemEvent_AllFields) {
    uint8_t buf[12];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'S';                              // message_type
    write_be16(buf + 1, 0);                    // stock_locate (always 0)
    write_be16(buf + 3, 1);                    // tracking_number
    write_be48(buf + 5, 34200000000000ULL);    // timestamp: 09:30:00 in nanos
    buf[11] = 'O';                             // event_code: Start of Messages

    auto* msg = reinterpret_cast<const SystemEvent*>(buf);

    EXPECT_EQ(msg->message_type(), 'S');
    EXPECT_EQ(msg->stock_locate(), 0);
    EXPECT_EQ(msg->tracking_number(), 1);
    EXPECT_EQ(msg->timestamp(), 34200000000000ULL);
    EXPECT_EQ(msg->event_code(), 'O');
}

TEST(SystemMessages, SystemEvent_Timestamp48bit_MaxValue) {
    uint8_t buf[12];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'S';
    // Max 48-bit value: 0xFFFFFFFFFFFF = 281474976710655
    constexpr uint64_t max48 = 0xFFFFFFFFFFFFULL;
    write_be48(buf + 5, max48);
    buf[11] = 'C';

    auto* msg = reinterpret_cast<const SystemEvent*>(buf);

    EXPECT_EQ(msg->timestamp(), max48);
    EXPECT_EQ(msg->event_code(), 'C');
}

// ── MWCBDeclineLevel ('V', 35B) ─────────────────────────────────────────────

TEST(SystemMessages, MWCBDeclineLevel_AllFields) {
    uint8_t buf[35];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'V';
    write_be16(buf + 1, 0);
    write_be16(buf + 3, 5);
    write_be48(buf + 5, 50000000000ULL);
    // level_1 = 350000000000 => $3500.00 (8 implied decimals)
    write_be64(buf + 11, 350000000000ULL);
    // level_2 = 280000000000 => $2800.00
    write_be64(buf + 19, 280000000000ULL);
    // level_3 = 210000000000 => $2100.00
    write_be64(buf + 27, 210000000000ULL);

    auto* msg = reinterpret_cast<const MWCBDeclineLevel*>(buf);

    EXPECT_EQ(msg->message_type(), 'V');
    EXPECT_EQ(msg->stock_locate(), 0);
    EXPECT_EQ(msg->tracking_number(), 5);
    EXPECT_EQ(msg->timestamp(), 50000000000ULL);

    EXPECT_EQ(msg->level_1(), 350000000000ULL);
    EXPECT_DOUBLE_EQ(msg->level_1_double(), 3500.0);

    EXPECT_EQ(msg->level_2(), 280000000000ULL);
    EXPECT_DOUBLE_EQ(msg->level_2_double(), 2800.0);

    EXPECT_EQ(msg->level_3(), 210000000000ULL);
    EXPECT_DOUBLE_EQ(msg->level_3_double(), 2100.0);
}

// ── MWCBStatus ('W', 12B) ───────────────────────────────────────────────────

TEST(SystemMessages, MWCBStatus_AllFields) {
    uint8_t buf[12];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'W';
    write_be16(buf + 1, 0);
    write_be16(buf + 3, 10);
    write_be48(buf + 5, 40000000000ULL);
    buf[11] = '1';  // Level 1 breached

    auto* msg = reinterpret_cast<const MWCBStatus*>(buf);

    EXPECT_EQ(msg->message_type(), 'W');
    EXPECT_EQ(msg->stock_locate(), 0);
    EXPECT_EQ(msg->tracking_number(), 10);
    EXPECT_EQ(msg->timestamp(), 40000000000ULL);
    EXPECT_EQ(msg->breached_level(), '1');
}

// =============================================================================
// STOCK-RELATED MESSAGES
// =============================================================================

// ── StockDirectory ('R', 39B) ───────────────────────────────────────────────

TEST(StockRelated, StockDirectory_AllFields) {
    uint8_t buf[39];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'R';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 7);
    write_be48(buf + 5, 34200000000000ULL);
    std::memcpy(buf + 11, "AAPL    ", 8);   // stock
    buf[19] = 'Q';                           // market_category: Nasdaq GSM
    buf[20] = 'N';                           // financial_status: Normal
    write_be32(buf + 21, 100);               // round_lot_size
    buf[25] = 'Y';                           // round_lots_only
    buf[26] = 'A';                           // issue_classification
    std::memcpy(buf + 27, "NA", 2);          // issue_sub_type
    buf[29] = 'P';                           // authenticity: Production
    buf[30] = 'N';                           // short_sale_threshold
    buf[31] = 'N';                           // ipo_flag
    buf[32] = '1';                           // luld_reference_price_tier
    buf[33] = 'N';                           // etp_flag
    write_be32(buf + 34, 0);                 // etp_leverage_factor
    buf[38] = 'N';                           // inverse_indicator

    auto* msg = reinterpret_cast<const StockDirectory*>(buf);

    EXPECT_EQ(msg->message_type(), 'R');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 7);
    EXPECT_EQ(msg->timestamp(), 34200000000000ULL);
    EXPECT_EQ(msg->stock(), "AAPL    ");
    EXPECT_EQ(msg->stock_trimmed(), "AAPL");
    EXPECT_EQ(msg->market_category(), 'Q');
    EXPECT_EQ(msg->financial_status_indicator(), 'N');
    EXPECT_EQ(msg->round_lot_size(), 100u);
    EXPECT_EQ(msg->round_lots_only(), 'Y');
    EXPECT_EQ(msg->issue_classification(), 'A');
    EXPECT_EQ(msg->issue_sub_type(), "NA");
    EXPECT_EQ(msg->issue_sub_type_trimmed(), "NA");
    EXPECT_EQ(msg->authenticity(), 'P');
    EXPECT_EQ(msg->short_sale_threshold_indicator(), 'N');
    EXPECT_EQ(msg->ipo_flag(), 'N');
    EXPECT_EQ(msg->luld_reference_price_tier(), '1');
    EXPECT_EQ(msg->etp_flag(), 'N');
    EXPECT_EQ(msg->etp_leverage_factor(), 0u);
    EXPECT_EQ(msg->inverse_indicator(), 'N');
}

TEST(StockRelated, StockDirectory_IssueSubType_Trimmed) {
    uint8_t buf[39];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'R';
    std::memcpy(buf + 11, "SPY     ", 8);
    // issue_sub_type = "Z " (single-char sub-type padded right)
    std::memcpy(buf + 27, "Z ", 2);

    auto* msg = reinterpret_cast<const StockDirectory*>(buf);

    EXPECT_EQ(msg->issue_sub_type(), "Z ");
    EXPECT_EQ(msg->issue_sub_type_trimmed(), "Z");
}

// ── StockTradingAction ('H', 25B) ───────────────────────────────────────────

TEST(StockRelated, StockTradingAction_AllFields) {
    uint8_t buf[25];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'H';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 99);
    write_be48(buf + 5, 50000000000ULL);
    std::memcpy(buf + 11, "MSFT    ", 8);
    buf[19] = 'H';                          // trading_state: Halted
    buf[20] = ' ';                          // reserved
    std::memcpy(buf + 21, "LUDP", 4);       // reason

    auto* msg = reinterpret_cast<const StockTradingAction*>(buf);

    EXPECT_EQ(msg->message_type(), 'H');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 99);
    EXPECT_EQ(msg->timestamp(), 50000000000ULL);
    EXPECT_EQ(msg->stock(), "MSFT    ");
    EXPECT_EQ(msg->stock_trimmed(), "MSFT");
    EXPECT_EQ(msg->trading_state(), 'H');
    EXPECT_EQ(msg->reserved(), ' ');
    EXPECT_EQ(msg->reason(), "LUDP");
    EXPECT_EQ(msg->reason_trimmed(), "LUDP");
}

TEST(StockRelated, StockTradingAction_Reason_Trimmed) {
    uint8_t buf[25];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'H';
    std::memcpy(buf + 21, "T1  ", 4);  // Padded reason

    auto* msg = reinterpret_cast<const StockTradingAction*>(buf);

    EXPECT_EQ(msg->reason(), "T1  ");
    EXPECT_EQ(msg->reason_trimmed(), "T1");
}

// ── RegSHORestriction ('Y', 20B) ────────────────────────────────────────────

TEST(StockRelated, RegSHORestriction_AllFields) {
    uint8_t buf[20];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'Y';
    write_be16(buf + 1, 100);
    write_be16(buf + 3, 200);
    write_be48(buf + 5, 60000000000ULL);
    std::memcpy(buf + 11, "GOOG    ", 8);
    buf[19] = '1';  // Reg SHO restriction in effect

    auto* msg = reinterpret_cast<const RegSHORestriction*>(buf);

    EXPECT_EQ(msg->message_type(), 'Y');
    EXPECT_EQ(msg->stock_locate(), 100);
    EXPECT_EQ(msg->tracking_number(), 200);
    EXPECT_EQ(msg->timestamp(), 60000000000ULL);
    EXPECT_EQ(msg->stock(), "GOOG    ");
    EXPECT_EQ(msg->stock_trimmed(), "GOOG");
    EXPECT_EQ(msg->reg_sho_action(), '1');
}

// ── MarketParticipantPosition ('L', 26B) ────────────────────────────────────

TEST(StockRelated, MarketParticipantPosition_AllFields) {
    uint8_t buf[26];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'L';
    write_be16(buf + 1, 50);
    write_be16(buf + 3, 77);
    write_be48(buf + 5, 45000000000ULL);
    std::memcpy(buf + 11, "GSCO", 4);       // mpid
    std::memcpy(buf + 15, "NVDA    ", 8);    // stock
    buf[23] = 'Y';                           // primary_market_maker
    buf[24] = 'N';                           // market_maker_mode: Normal
    buf[25] = 'A';                           // market_participant_state: Active

    auto* msg = reinterpret_cast<const MarketParticipantPosition*>(buf);

    EXPECT_EQ(msg->message_type(), 'L');
    EXPECT_EQ(msg->stock_locate(), 50);
    EXPECT_EQ(msg->tracking_number(), 77);
    EXPECT_EQ(msg->timestamp(), 45000000000ULL);
    EXPECT_EQ(msg->mpid(), "GSCO");
    EXPECT_EQ(msg->mpid_trimmed(), "GSCO");
    EXPECT_EQ(msg->stock(), "NVDA    ");
    EXPECT_EQ(msg->stock_trimmed(), "NVDA");
    EXPECT_EQ(msg->primary_market_maker(), 'Y');
    EXPECT_EQ(msg->market_maker_mode(), 'N');
    EXPECT_EQ(msg->market_participant_state(), 'A');
}

TEST(StockRelated, MarketParticipantPosition_MPID_Trimmed) {
    uint8_t buf[26];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'L';
    std::memcpy(buf + 11, "ML  ", 4);  // 2-char MPID padded

    auto* msg = reinterpret_cast<const MarketParticipantPosition*>(buf);

    EXPECT_EQ(msg->mpid(), "ML  ");
    EXPECT_EQ(msg->mpid_trimmed(), "ML");
}

// ── IPOQuotingPeriodUpdate ('K', 28B) ───────────────────────────────────────

TEST(StockRelated, IPOQuotingPeriodUpdate_AllFields) {
    uint8_t buf[28];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'K';
    write_be16(buf + 1, 300);
    write_be16(buf + 3, 8);
    write_be48(buf + 5, 34200000000000ULL);
    std::memcpy(buf + 11, "NEWIPO  ", 8);
    write_be32(buf + 19, 37800);       // ipo_quotation_release_time: seconds
    buf[23] = 'A';                      // qualifier: Anticipated
    write_be32(buf + 24, 250000);       // ipo_price: $25.00 (4 decimals)

    auto* msg = reinterpret_cast<const IPOQuotingPeriodUpdate*>(buf);

    EXPECT_EQ(msg->message_type(), 'K');
    EXPECT_EQ(msg->stock_locate(), 300);
    EXPECT_EQ(msg->tracking_number(), 8);
    EXPECT_EQ(msg->timestamp(), 34200000000000ULL);
    EXPECT_EQ(msg->stock(), "NEWIPO  ");
    EXPECT_EQ(msg->stock_trimmed(), "NEWIPO");
    EXPECT_EQ(msg->ipo_quotation_release_time(), 37800u);
    EXPECT_EQ(msg->ipo_quotation_release_qualifier(), 'A');
    EXPECT_EQ(msg->ipo_price(), 250000u);
    EXPECT_DOUBLE_EQ(msg->ipo_price_double(), 25.0);
}

// ── LULDAuctionCollar ('J', 35B) ────────────────────────────────────────────

TEST(StockRelated, LULDAuctionCollar_AllFields) {
    uint8_t buf[35];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'J';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 12);
    write_be48(buf + 5, 55000000000ULL);
    std::memcpy(buf + 11, "AMZN    ", 8);
    write_be32(buf + 19, 1500000);     // ref price: $150.0000
    write_be32(buf + 23, 1575000);     // upper collar: $157.5000
    write_be32(buf + 27, 1425000);     // lower collar: $142.5000
    write_be32(buf + 31, 3);           // auction_collar_extension

    auto* msg = reinterpret_cast<const LULDAuctionCollar*>(buf);

    EXPECT_EQ(msg->message_type(), 'J');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 12);
    EXPECT_EQ(msg->timestamp(), 55000000000ULL);
    EXPECT_EQ(msg->stock(), "AMZN    ");
    EXPECT_EQ(msg->stock_trimmed(), "AMZN");

    EXPECT_EQ(msg->auction_collar_reference_price(), 1500000u);
    EXPECT_DOUBLE_EQ(msg->auction_collar_reference_price_double(), 150.0);

    EXPECT_EQ(msg->upper_auction_collar_price(), 1575000u);
    EXPECT_DOUBLE_EQ(msg->upper_auction_collar_price_double(), 157.5);

    EXPECT_EQ(msg->lower_auction_collar_price(), 1425000u);
    EXPECT_DOUBLE_EQ(msg->lower_auction_collar_price_double(), 142.5);

    EXPECT_EQ(msg->auction_collar_extension(), 3u);
}

// ── OperationalHalt ('h', 21B) ──────────────────────────────────────────────

TEST(StockRelated, OperationalHalt_AllFields) {
    uint8_t buf[21];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'h';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 15);
    write_be48(buf + 5, 36000000000000ULL);
    std::memcpy(buf + 11, "META    ", 8);
    buf[19] = 'Q';  // market_code: Nasdaq
    buf[20] = 'H';  // operational_halt_action: Halted

    auto* msg = reinterpret_cast<const OperationalHalt*>(buf);

    EXPECT_EQ(msg->message_type(), 'h');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 15);
    EXPECT_EQ(msg->timestamp(), 36000000000000ULL);
    EXPECT_EQ(msg->stock(), "META    ");
    EXPECT_EQ(msg->stock_trimmed(), "META");
    EXPECT_EQ(msg->market_code(), 'Q');
    EXPECT_EQ(msg->operational_halt_action(), 'H');
}

// =============================================================================
// ADD ORDER MESSAGES
// =============================================================================

// ── AddOrder ('A', 36B) ─────────────────────────────────────────────────────

TEST(AddOrderMessages, AddOrder_AllFields) {
    uint8_t buf[36];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'A';
    write_be16(buf + 1, 101);                          // locate
    write_be16(buf + 3, 202);                          // tracking
    write_be48(buf + 5, 30303030);                     // timestamp
    write_be64(buf + 11, 0x1234567890ABCDEFULL);       // ref num
    buf[19] = 'S';                                     // sell
    write_be32(buf + 20, 500);                         // shares
    std::memcpy(buf + 24, "TSLA    ", 8);              // stock
    write_be32(buf + 32, 2500000);                     // price $250.00

    auto* msg = reinterpret_cast<const AddOrder*>(buf);

    EXPECT_EQ(msg->message_type(), 'A');
    EXPECT_EQ(msg->stock_locate(), 101);
    EXPECT_EQ(msg->tracking_number(), 202);
    EXPECT_EQ(msg->timestamp(), 30303030u);
    EXPECT_EQ(msg->order_reference_number(), 0x1234567890ABCDEFULL);
    EXPECT_EQ(msg->buy_sell_indicator(), 'S');
    EXPECT_EQ(msg->shares(), 500u);
    EXPECT_EQ(msg->stock(), "TSLA    ");
    EXPECT_EQ(msg->stock_trimmed(), "TSLA");
    EXPECT_EQ(msg->price(), 2500000u);
    EXPECT_DOUBLE_EQ(msg->price_double(), 250.00);
}

TEST(AddOrderMessages, AddOrder_BuySide_PriceFractional) {
    uint8_t buf[36];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'A';
    buf[19] = 'B';                         // buy
    write_be32(buf + 20, 1000);            // shares
    std::memcpy(buf + 24, "AAPL    ", 8);
    write_be32(buf + 32, 1734500);         // price $173.45

    auto* msg = reinterpret_cast<const AddOrder*>(buf);

    EXPECT_EQ(msg->buy_sell_indicator(), 'B');
    EXPECT_EQ(msg->shares(), 1000u);
    EXPECT_EQ(msg->stock_trimmed(), "AAPL");
    EXPECT_EQ(msg->price(), 1734500u);
    EXPECT_DOUBLE_EQ(msg->price_double(), 173.45);
}

// ── AddOrderMPIDAttribution ('F', 40B) ──────────────────────────────────────

TEST(AddOrderMessages, AddOrderMPIDAttribution_AllFields) {
    uint8_t buf[40];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'F';
    write_be16(buf + 1, 55);
    write_be16(buf + 3, 10);
    write_be48(buf + 5, 35000000000ULL);
    write_be64(buf + 11, 9876543210ULL);   // order ref
    buf[19] = 'B';                          // buy
    write_be32(buf + 20, 200);              // shares
    std::memcpy(buf + 24, "MSFT    ", 8);   // stock
    write_be32(buf + 32, 3400000);          // price $340.00
    std::memcpy(buf + 36, "GSCO", 4);       // attribution (MPID)

    auto* msg = reinterpret_cast<const AddOrderMPIDAttribution*>(buf);

    EXPECT_EQ(msg->message_type(), 'F');
    EXPECT_EQ(msg->stock_locate(), 55);
    EXPECT_EQ(msg->tracking_number(), 10);
    EXPECT_EQ(msg->timestamp(), 35000000000ULL);
    EXPECT_EQ(msg->order_reference_number(), 9876543210ULL);
    EXPECT_EQ(msg->buy_sell_indicator(), 'B');
    EXPECT_EQ(msg->shares(), 200u);
    EXPECT_EQ(msg->stock(), "MSFT    ");
    EXPECT_EQ(msg->stock_trimmed(), "MSFT");
    EXPECT_EQ(msg->price(), 3400000u);
    EXPECT_DOUBLE_EQ(msg->price_double(), 340.00);
    EXPECT_EQ(msg->attribution(), "GSCO");
    EXPECT_EQ(msg->attribution_trimmed(), "GSCO");
}

TEST(AddOrderMessages, AddOrderMPIDAttribution_Attribution_Trimmed) {
    uint8_t buf[40];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'F';
    std::memcpy(buf + 36, "ML  ", 4);  // 2-char MPID padded

    auto* msg = reinterpret_cast<const AddOrderMPIDAttribution*>(buf);

    EXPECT_EQ(msg->attribution(), "ML  ");
    EXPECT_EQ(msg->attribution_trimmed(), "ML");
}

// =============================================================================
// MODIFY ORDER MESSAGES
// =============================================================================

// ── OrderExecuted ('E', 31B) ────────────────────────────────────────────────

TEST(ModifyOrderMessages, OrderExecuted_AllFields) {
    uint8_t buf[31];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'E';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 3);
    write_be48(buf + 5, 36100000000000ULL);
    write_be64(buf + 11, 1111111111ULL);    // order ref
    write_be32(buf + 19, 100);              // executed_shares
    write_be64(buf + 23, 9999999999ULL);    // match_number

    auto* msg = reinterpret_cast<const OrderExecuted*>(buf);

    EXPECT_EQ(msg->message_type(), 'E');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 3);
    EXPECT_EQ(msg->timestamp(), 36100000000000ULL);
    EXPECT_EQ(msg->order_reference_number(), 1111111111ULL);
    EXPECT_EQ(msg->executed_shares(), 100u);
    EXPECT_EQ(msg->match_number(), 9999999999ULL);
}

// ── OrderExecutedWithPrice ('C', 36B) ───────────────────────────────────────

TEST(ModifyOrderMessages, OrderExecutedWithPrice_AllFields) {
    uint8_t buf[36];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'C';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 4);
    write_be48(buf + 5, 36200000000000ULL);
    write_be64(buf + 11, 2222222222ULL);    // order ref
    write_be32(buf + 19, 50);               // executed_shares
    write_be64(buf + 23, 5555555555ULL);    // match_number
    buf[31] = 'Y';                          // printable
    write_be32(buf + 32, 1505000);          // execution_price $150.50

    auto* msg = reinterpret_cast<const OrderExecutedWithPrice*>(buf);

    EXPECT_EQ(msg->message_type(), 'C');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 4);
    EXPECT_EQ(msg->timestamp(), 36200000000000ULL);
    EXPECT_EQ(msg->order_reference_number(), 2222222222ULL);
    EXPECT_EQ(msg->executed_shares(), 50u);
    EXPECT_EQ(msg->match_number(), 5555555555ULL);
    EXPECT_EQ(msg->printable(), 'Y');
    EXPECT_EQ(msg->execution_price(), 1505000u);
    EXPECT_DOUBLE_EQ(msg->execution_price_double(), 150.50);
}

TEST(ModifyOrderMessages, OrderExecutedWithPrice_NonPrintable) {
    uint8_t buf[36];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'C';
    buf[31] = 'N';  // Non-printable

    auto* msg = reinterpret_cast<const OrderExecutedWithPrice*>(buf);

    EXPECT_EQ(msg->printable(), 'N');
}

// ── OrderCancel ('X', 23B) ──────────────────────────────────────────────────

TEST(ModifyOrderMessages, OrderCancel_AllFields) {
    uint8_t buf[23];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'X';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 5);
    write_be48(buf + 5, 36300000000000ULL);
    write_be64(buf + 11, 3333333333ULL);   // order ref
    write_be32(buf + 19, 75);              // cancelled_shares

    auto* msg = reinterpret_cast<const OrderCancel*>(buf);

    EXPECT_EQ(msg->message_type(), 'X');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 5);
    EXPECT_EQ(msg->timestamp(), 36300000000000ULL);
    EXPECT_EQ(msg->order_reference_number(), 3333333333ULL);
    EXPECT_EQ(msg->cancelled_shares(), 75u);
}

// ── OrderDelete ('D', 19B) ──────────────────────────────────────────────────

TEST(ModifyOrderMessages, OrderDelete_AllFields) {
    uint8_t buf[19];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'D';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 6);
    write_be48(buf + 5, 36400000000000ULL);
    write_be64(buf + 11, 4444444444ULL);   // order ref

    auto* msg = reinterpret_cast<const OrderDelete*>(buf);

    EXPECT_EQ(msg->message_type(), 'D');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 6);
    EXPECT_EQ(msg->timestamp(), 36400000000000ULL);
    EXPECT_EQ(msg->order_reference_number(), 4444444444ULL);
}

// ── OrderReplace ('U', 35B) ─────────────────────────────────────────────────

TEST(ModifyOrderMessages, OrderReplace_AllFields) {
    uint8_t buf[35];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'U';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 7);
    write_be48(buf + 5, 36500000000000ULL);
    write_be64(buf + 11, 1000000001ULL);   // original_order_reference_number
    write_be64(buf + 19, 1000000002ULL);   // new_order_reference_number
    write_be32(buf + 27, 300);             // shares
    write_be32(buf + 31, 1600000);         // price $160.00

    auto* msg = reinterpret_cast<const OrderReplace*>(buf);

    EXPECT_EQ(msg->message_type(), 'U');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 7);
    EXPECT_EQ(msg->timestamp(), 36500000000000ULL);
    EXPECT_EQ(msg->original_order_reference_number(), 1000000001ULL);
    EXPECT_EQ(msg->new_order_reference_number(), 1000000002ULL);
    EXPECT_EQ(msg->shares(), 300u);
    EXPECT_EQ(msg->price(), 1600000u);
    EXPECT_DOUBLE_EQ(msg->price_double(), 160.00);
}

// =============================================================================
// TRADE MESSAGES
// =============================================================================

// ── TradeNonCross ('P', 44B) ────────────────────────────────────────────────

TEST(TradeMessages, TradeNonCross_AllFields) {
    uint8_t buf[44];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'P';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 20);
    write_be48(buf + 5, 37000000000000ULL);
    write_be64(buf + 11, 7777777777ULL);   // order ref
    buf[19] = 'B';                          // buy
    write_be32(buf + 20, 150);              // shares
    std::memcpy(buf + 24, "NFLX    ", 8);   // stock
    write_be32(buf + 32, 4505000);          // price $450.50
    write_be64(buf + 36, 8888888888ULL);    // match_number

    auto* msg = reinterpret_cast<const TradeNonCross*>(buf);

    EXPECT_EQ(msg->message_type(), 'P');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 20);
    EXPECT_EQ(msg->timestamp(), 37000000000000ULL);
    EXPECT_EQ(msg->order_reference_number(), 7777777777ULL);
    EXPECT_EQ(msg->buy_sell_indicator(), 'B');
    EXPECT_EQ(msg->shares(), 150u);
    EXPECT_EQ(msg->stock(), "NFLX    ");
    EXPECT_EQ(msg->stock_trimmed(), "NFLX");
    EXPECT_EQ(msg->price(), 4505000u);
    EXPECT_DOUBLE_EQ(msg->price_double(), 450.50);
    EXPECT_EQ(msg->match_number(), 8888888888ULL);
}

// ── CrossTrade ('Q', 40B) ───────────────────────────────────────────────────

TEST(TradeMessages, CrossTrade_AllFields) {
    uint8_t buf[40];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'Q';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 25);
    write_be48(buf + 5, 57600000000000ULL);    // 16:00:00 in nanos
    write_be64(buf + 11, 5000000ULL);          // shares (int64!)
    std::memcpy(buf + 19, "SPY     ", 8);      // stock
    write_be32(buf + 27, 4500000);             // cross_price $450.00
    write_be64(buf + 31, 1234567890ULL);       // match_number
    buf[39] = 'C';                             // cross_type: Closing

    auto* msg = reinterpret_cast<const CrossTrade*>(buf);

    EXPECT_EQ(msg->message_type(), 'Q');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 25);
    EXPECT_EQ(msg->timestamp(), 57600000000000ULL);
    EXPECT_EQ(msg->shares(), 5000000ULL);
    EXPECT_EQ(msg->stock(), "SPY     ");
    EXPECT_EQ(msg->stock_trimmed(), "SPY");
    EXPECT_EQ(msg->cross_price(), 4500000u);
    EXPECT_DOUBLE_EQ(msg->cross_price_double(), 450.00);
    EXPECT_EQ(msg->match_number(), 1234567890ULL);
    EXPECT_EQ(msg->cross_type(), 'C');
}

TEST(TradeMessages, CrossTrade_OpeningCross) {
    uint8_t buf[40];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'Q';
    buf[39] = 'O';  // Opening Cross

    auto* msg = reinterpret_cast<const CrossTrade*>(buf);

    EXPECT_EQ(msg->cross_type(), 'O');
}

// ── BrokenTrade ('B', 19B) ──────────────────────────────────────────────────

TEST(TradeMessages, BrokenTrade_AllFields) {
    uint8_t buf[19];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'B';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 30);
    write_be48(buf + 5, 38000000000000ULL);
    write_be64(buf + 11, 6666666666ULL);   // match_number

    auto* msg = reinterpret_cast<const BrokenTrade*>(buf);

    EXPECT_EQ(msg->message_type(), 'B');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 30);
    EXPECT_EQ(msg->timestamp(), 38000000000000ULL);
    EXPECT_EQ(msg->match_number(), 6666666666ULL);
}

// =============================================================================
// NOII MESSAGES
// =============================================================================

// ── NOII ('I', 50B) ─────────────────────────────────────────────────────────

TEST(NOIIMessages, NOII_AllFields) {
    uint8_t buf[50];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'I';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 40);
    write_be48(buf + 5, 34200000000000ULL);
    write_be64(buf + 11, 10000ULL);         // paired_shares
    write_be64(buf + 19, 500ULL);           // imbalance_shares
    buf[27] = 'B';                           // imbalance_direction: Buy
    std::memcpy(buf + 28, "QQQ     ", 8);    // stock
    write_be32(buf + 36, 3700000);           // far_price $370.00
    write_be32(buf + 40, 3695000);           // near_price $369.50
    write_be32(buf + 44, 3698000);           // current_reference_price $369.80
    buf[48] = 'O';                           // cross_type: Opening
    buf[49] = 'L';                           // price_variation_indicator

    auto* msg = reinterpret_cast<const NOII*>(buf);

    EXPECT_EQ(msg->message_type(), 'I');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 40);
    EXPECT_EQ(msg->timestamp(), 34200000000000ULL);
    EXPECT_EQ(msg->paired_shares(), 10000ULL);
    EXPECT_EQ(msg->imbalance_shares(), 500ULL);
    EXPECT_EQ(msg->imbalance_direction(), 'B');
    EXPECT_EQ(msg->stock(), "QQQ     ");
    EXPECT_EQ(msg->stock_trimmed(), "QQQ");

    EXPECT_EQ(msg->far_price(), 3700000u);
    EXPECT_DOUBLE_EQ(msg->far_price_double(), 370.00);

    EXPECT_EQ(msg->near_price(), 3695000u);
    EXPECT_DOUBLE_EQ(msg->near_price_double(), 369.50);

    EXPECT_EQ(msg->current_reference_price(), 3698000u);
    EXPECT_DOUBLE_EQ(msg->current_reference_price_double(), 369.80);

    EXPECT_EQ(msg->cross_type(), 'O');
    EXPECT_EQ(msg->price_variation_indicator(), 'L');
}

// ── RetailInterest ('N', 20B) ───────────────────────────────────────────────

TEST(NOIIMessages, RetailInterest_AllFields) {
    uint8_t buf[20];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'N';
    write_be16(buf + 1, 42);
    write_be16(buf + 3, 50);
    write_be48(buf + 5, 39000000000000ULL);
    std::memcpy(buf + 11, "TSLA    ", 8);
    buf[19] = 'A';  // interest on both sides

    auto* msg = reinterpret_cast<const RetailInterest*>(buf);

    EXPECT_EQ(msg->message_type(), 'N');
    EXPECT_EQ(msg->stock_locate(), 42);
    EXPECT_EQ(msg->tracking_number(), 50);
    EXPECT_EQ(msg->timestamp(), 39000000000000ULL);
    EXPECT_EQ(msg->stock(), "TSLA    ");
    EXPECT_EQ(msg->stock_trimmed(), "TSLA");
    EXPECT_EQ(msg->interest_flag(), 'A');
}

TEST(NOIIMessages, RetailInterest_NoInterest) {
    uint8_t buf[20];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'N';
    buf[19] = 'N';  // No retail interest

    auto* msg = reinterpret_cast<const RetailInterest*>(buf);

    EXPECT_EQ(msg->interest_flag(), 'N');
}

// =============================================================================
// TIMESTAMP AND EDGE CASE TESTS
// =============================================================================

TEST(TimestampTests, Timestamp48bit_ReconstructionFromBytes) {
    // Manually set 6 bytes and verify the 48-bit reader reconstructs correctly
    // Value: 0xAB_CD_EF_12_34_56 = 188900977004630
    uint8_t buf[12];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'S';
    buf[5] = 0xAB;
    buf[6] = 0xCD;
    buf[7] = 0xEF;
    buf[8] = 0x12;
    buf[9] = 0x34;
    buf[10] = 0x56;

    auto* msg = reinterpret_cast<const SystemEvent*>(buf);

    uint64_t expected = (uint64_t(0xAB) << 40) | (uint64_t(0xCD) << 32) |
                        (uint64_t(0xEF) << 24) | (uint64_t(0x12) << 16) |
                        (uint64_t(0x34) << 8)  |  uint64_t(0x56);
    EXPECT_EQ(expected, 0xABCDEF123456ULL);
    EXPECT_EQ(msg->timestamp(), expected);
}

TEST(TimestampTests, Timestamp48bit_Zero) {
    uint8_t buf[12];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'S';

    auto* msg = reinterpret_cast<const SystemEvent*>(buf);

    EXPECT_EQ(msg->timestamp(), 0ULL);
}

TEST(TimestampTests, Timestamp48bit_MidnightPlus1ns) {
    uint8_t buf[12];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'S';
    write_be48(buf + 5, 1);

    auto* msg = reinterpret_cast<const SystemEvent*>(buf);

    EXPECT_EQ(msg->timestamp(), 1ULL);
}

// =============================================================================
// STOCK SYMBOL TRIMMING EDGE CASES
// =============================================================================

TEST(AlphaFieldTests, Stock_FullyPadded_ReturnsEmpty) {
    // A stock field of all spaces should trim to empty
    uint8_t buf[20];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'Y';
    std::memcpy(buf + 11, "        ", 8);  // all spaces

    auto* msg = reinterpret_cast<const RegSHORestriction*>(buf);

    EXPECT_EQ(msg->stock(), "        ");
    EXPECT_EQ(msg->stock_trimmed(), "");
    EXPECT_TRUE(msg->stock_trimmed().empty());
}

TEST(AlphaFieldTests, Stock_ExactlyFilled) {
    // An 8-char symbol with no padding
    uint8_t buf[20];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'Y';
    std::memcpy(buf + 11, "ABCDEFGH", 8);

    auto* msg = reinterpret_cast<const RegSHORestriction*>(buf);

    EXPECT_EQ(msg->stock(), "ABCDEFGH");
    EXPECT_EQ(msg->stock_trimmed(), "ABCDEFGH");
}

TEST(AlphaFieldTests, Stock_SingleChar) {
    uint8_t buf[20];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'Y';
    std::memcpy(buf + 11, "X       ", 8);

    auto* msg = reinterpret_cast<const RegSHORestriction*>(buf);

    EXPECT_EQ(msg->stock_trimmed(), "X");
}

// =============================================================================
// PRICE CONVERSION EDGE CASES
// =============================================================================

TEST(PriceTests, Price4_Zero) {
    uint8_t buf[36];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'A';
    write_be32(buf + 32, 0);

    auto* msg = reinterpret_cast<const AddOrder*>(buf);

    EXPECT_EQ(msg->price(), 0u);
    EXPECT_DOUBLE_EQ(msg->price_double(), 0.0);
}

TEST(PriceTests, Price4_OnePenny) {
    uint8_t buf[36];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'A';
    write_be32(buf + 32, 100);  // $0.01

    auto* msg = reinterpret_cast<const AddOrder*>(buf);

    EXPECT_EQ(msg->price(), 100u);
    EXPECT_DOUBLE_EQ(msg->price_double(), 0.01);
}

TEST(PriceTests, Price4_MaxValue) {
    uint8_t buf[36];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'A';
    write_be32(buf + 32, 0xFFFFFFFF);

    auto* msg = reinterpret_cast<const AddOrder*>(buf);

    EXPECT_EQ(msg->price(), 0xFFFFFFFFu);
    EXPECT_DOUBLE_EQ(msg->price_double(), 4294967295.0 * 0.0001);
}

TEST(PriceTests, Price8_DeclineLevel_SubDollar) {
    uint8_t buf[35];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'V';
    // level_1 = 50000000 => $0.50 (8 implied decimals)
    write_be64(buf + 11, 50000000ULL);

    auto* msg = reinterpret_cast<const MWCBDeclineLevel*>(buf);

    EXPECT_EQ(msg->level_1(), 50000000ULL);
    EXPECT_DOUBLE_EQ(msg->level_1_double(), 0.5);
}

// =============================================================================
// STRUCT SIZE STATIC ASSERTIONS (compile-time, but good to verify in tests)
// =============================================================================

TEST(StructSizes, AllMessageSizesMatchSchema) {
    EXPECT_EQ(sizeof(SystemEvent), 12u);
    EXPECT_EQ(sizeof(StockDirectory), 39u);
    EXPECT_EQ(sizeof(StockTradingAction), 25u);
    EXPECT_EQ(sizeof(RegSHORestriction), 20u);
    EXPECT_EQ(sizeof(MarketParticipantPosition), 26u);
    EXPECT_EQ(sizeof(MWCBDeclineLevel), 35u);
    EXPECT_EQ(sizeof(MWCBStatus), 12u);
    EXPECT_EQ(sizeof(IPOQuotingPeriodUpdate), 28u);
    EXPECT_EQ(sizeof(LULDAuctionCollar), 35u);
    EXPECT_EQ(sizeof(OperationalHalt), 21u);
    EXPECT_EQ(sizeof(AddOrder), 36u);
    EXPECT_EQ(sizeof(AddOrderMPIDAttribution), 40u);
    EXPECT_EQ(sizeof(OrderExecuted), 31u);
    EXPECT_EQ(sizeof(OrderExecutedWithPrice), 36u);
    EXPECT_EQ(sizeof(OrderCancel), 23u);
    EXPECT_EQ(sizeof(OrderDelete), 19u);
    EXPECT_EQ(sizeof(OrderReplace), 35u);
    EXPECT_EQ(sizeof(TradeNonCross), 44u);
    EXPECT_EQ(sizeof(CrossTrade), 40u);
    EXPECT_EQ(sizeof(BrokenTrade), 19u);
    EXPECT_EQ(sizeof(NOII), 50u);
    EXPECT_EQ(sizeof(RetailInterest), 20u);
}

// =============================================================================
// MESSAGE SIZE / NAME LOOKUP
// =============================================================================

TEST(LookupTests, MessageSize_AllTypes) {
    EXPECT_EQ(message_size('S'), 12u);
    EXPECT_EQ(message_size('R'), 39u);
    EXPECT_EQ(message_size('H'), 25u);
    EXPECT_EQ(message_size('Y'), 20u);
    EXPECT_EQ(message_size('L'), 26u);
    EXPECT_EQ(message_size('V'), 35u);
    EXPECT_EQ(message_size('W'), 12u);
    EXPECT_EQ(message_size('K'), 28u);
    EXPECT_EQ(message_size('J'), 35u);
    EXPECT_EQ(message_size('h'), 21u);
    EXPECT_EQ(message_size('A'), 36u);
    EXPECT_EQ(message_size('F'), 40u);
    EXPECT_EQ(message_size('E'), 31u);
    EXPECT_EQ(message_size('C'), 36u);
    EXPECT_EQ(message_size('X'), 23u);
    EXPECT_EQ(message_size('D'), 19u);
    EXPECT_EQ(message_size('U'), 35u);
    EXPECT_EQ(message_size('P'), 44u);
    EXPECT_EQ(message_size('Q'), 40u);
    EXPECT_EQ(message_size('B'), 19u);
    EXPECT_EQ(message_size('I'), 50u);
    EXPECT_EQ(message_size('N'), 20u);
    EXPECT_EQ(message_size('Z'), 0u);  // unknown type
}

TEST(LookupTests, MessageName_AllTypes) {
    EXPECT_STREQ(message_name('S'), "SystemEvent");
    EXPECT_STREQ(message_name('R'), "StockDirectory");
    EXPECT_STREQ(message_name('H'), "StockTradingAction");
    EXPECT_STREQ(message_name('Y'), "RegSHORestriction");
    EXPECT_STREQ(message_name('L'), "MarketParticipantPosition");
    EXPECT_STREQ(message_name('V'), "MWCBDeclineLevel");
    EXPECT_STREQ(message_name('W'), "MWCBStatus");
    EXPECT_STREQ(message_name('K'), "IPOQuotingPeriodUpdate");
    EXPECT_STREQ(message_name('J'), "LULDAuctionCollar");
    EXPECT_STREQ(message_name('h'), "OperationalHalt");
    EXPECT_STREQ(message_name('A'), "AddOrder");
    EXPECT_STREQ(message_name('F'), "AddOrderMPIDAttribution");
    EXPECT_STREQ(message_name('E'), "OrderExecuted");
    EXPECT_STREQ(message_name('C'), "OrderExecutedWithPrice");
    EXPECT_STREQ(message_name('X'), "OrderCancel");
    EXPECT_STREQ(message_name('D'), "OrderDelete");
    EXPECT_STREQ(message_name('U'), "OrderReplace");
    EXPECT_STREQ(message_name('P'), "TradeNonCross");
    EXPECT_STREQ(message_name('Q'), "CrossTrade");
    EXPECT_STREQ(message_name('B'), "BrokenTrade");
    EXPECT_STREQ(message_name('I'), "NOII");
    EXPECT_STREQ(message_name('N'), "RetailInterest");
    EXPECT_STREQ(message_name('Z'), "Unknown");
}

TEST(LookupTests, MessageTypeConstants) {
    EXPECT_EQ(msg::SystemEvent, 'S');
    EXPECT_EQ(msg::StockDirectory, 'R');
    EXPECT_EQ(msg::StockTradingAction, 'H');
    EXPECT_EQ(msg::RegSHORestriction, 'Y');
    EXPECT_EQ(msg::MarketParticipantPosition, 'L');
    EXPECT_EQ(msg::MWCBDeclineLevel, 'V');
    EXPECT_EQ(msg::MWCBStatus, 'W');
    EXPECT_EQ(msg::IPOQuotingPeriodUpdate, 'K');
    EXPECT_EQ(msg::LULDAuctionCollar, 'J');
    EXPECT_EQ(msg::OperationalHalt, 'h');
    EXPECT_EQ(msg::AddOrder, 'A');
    EXPECT_EQ(msg::AddOrderMPIDAttribution, 'F');
    EXPECT_EQ(msg::OrderExecuted, 'E');
    EXPECT_EQ(msg::OrderExecutedWithPrice, 'C');
    EXPECT_EQ(msg::OrderCancel, 'X');
    EXPECT_EQ(msg::OrderDelete, 'D');
    EXPECT_EQ(msg::OrderReplace, 'U');
    EXPECT_EQ(msg::TradeNonCross, 'P');
    EXPECT_EQ(msg::CrossTrade, 'Q');
    EXPECT_EQ(msg::BrokenTrade, 'B');
    EXPECT_EQ(msg::NOII, 'I');
    EXPECT_EQ(msg::RetailInterest, 'N');

    EXPECT_EQ(NUM_MESSAGE_TYPES, 22u);
    EXPECT_EQ(MAX_MESSAGE_SIZE, 50u);
}

// =============================================================================
// DISPATCH TESTS
// =============================================================================

// A mock handler to test the dispatching mechanisms
struct MockHandler {
    int add_orders = 0;
    int deletes = 0;
    int unhandled = 0;

    void handle(const AddOrder& msg) {
        add_orders++;
        EXPECT_EQ(msg.stock_trimmed(), "MSFT");
    }

    void handle(const OrderDelete& msg) {
        deletes++;
        EXPECT_EQ(msg.order_reference_number(), 42u);
    }
};

TEST(DispatchTests, StaticDispatcher) {
    MockHandler handler;

    uint8_t add_buf[36] = {0};
    add_buf[0] = 'A';
    std::memcpy(add_buf + 24, "MSFT    ", 8);

    uint8_t del_buf[19] = {0};
    del_buf[0] = 'D';
    write_be64(del_buf + 11, 42);

    uint8_t exec_buf[31] = {0};
    exec_buf[0] = 'E'; // MockHandler doesn't handle this

    EXPECT_TRUE(dispatch_message(handler, 'A', reinterpret_cast<char*>(add_buf)));
    EXPECT_TRUE(dispatch_message(handler, 'D', reinterpret_cast<char*>(del_buf)));

    // 'E' is unhandled. dispatch_message returns true because 'E' is a valid
    // protocol message, but it compiles to a no-op branch.
    EXPECT_TRUE(dispatch_message(handler, 'E', reinterpret_cast<char*>(exec_buf)));

    // Invalid type
    EXPECT_FALSE(dispatch_message(handler, 'Z', reinterpret_cast<char*>(add_buf)));

    EXPECT_EQ(handler.add_orders, 1);
    EXPECT_EQ(handler.deletes, 1);
}

TEST(DispatchTests, O1_DispatchTable) {
    MockHandler handler;
    DispatchTable<MockHandler> table;

    uint8_t add_buf[36] = {0};
    add_buf[0] = 'A';
    std::memcpy(add_buf + 24, "MSFT    ", 8);

    uint8_t exec_buf[31] = {0};
    exec_buf[0] = 'E';

    EXPECT_TRUE(table.dispatch(handler, 'A', reinterpret_cast<char*>(add_buf)));

    // 'E' is unhandled by MockHandler, so its slot in the table is null.
    EXPECT_FALSE(table.dispatch(handler, 'E', reinterpret_cast<char*>(exec_buf)));

    EXPECT_EQ(handler.add_orders, 1);
}

TEST(DispatchTests, StaticDispatcher_AllValidTypes_ReturnTrue) {
    // Verify every valid message type returns true (even if unhandled)
    // Use a zeroed buffer — we only care about dispatch returning true,
    // not field values, so we use a handler with no field assertions.
    struct NoOpHandler {
        void handle(const SystemEvent&) {}
        void handle(const StockDirectory&) {}
        void handle(const StockTradingAction&) {}
        void handle(const RegSHORestriction&) {}
        void handle(const MarketParticipantPosition&) {}
        void handle(const MWCBDeclineLevel&) {}
        void handle(const MWCBStatus&) {}
        void handle(const IPOQuotingPeriodUpdate&) {}
        void handle(const LULDAuctionCollar&) {}
        void handle(const OperationalHalt&) {}
        void handle(const AddOrder&) {}
        void handle(const AddOrderMPIDAttribution&) {}
        void handle(const OrderExecuted&) {}
        void handle(const OrderExecutedWithPrice&) {}
        void handle(const OrderCancel&) {}
        void handle(const OrderDelete&) {}
        void handle(const OrderReplace&) {}
        void handle(const TradeNonCross&) {}
        void handle(const CrossTrade&) {}
        void handle(const BrokenTrade&) {}
        void handle(const NOII&) {}
        void handle(const RetailInterest&) {}
    };

    NoOpHandler handler;
    uint8_t buf[50] = {0};  // Largest message

    for (char type : ALL_MESSAGE_TYPES) {
        buf[0] = type;
        EXPECT_TRUE(dispatch_message(handler, type, reinterpret_cast<char*>(buf)))
            << "dispatch_message returned false for valid type '" << type << "'";
    }
}

// A comprehensive handler that handles every type to test the full dispatch table
struct FullHandler {
    int counts[256] = {};

    void handle(const SystemEvent&)               { counts[static_cast<uint8_t>('S')]++; }
    void handle(const StockDirectory&)             { counts[static_cast<uint8_t>('R')]++; }
    void handle(const StockTradingAction&)         { counts[static_cast<uint8_t>('H')]++; }
    void handle(const RegSHORestriction&)          { counts[static_cast<uint8_t>('Y')]++; }
    void handle(const MarketParticipantPosition&)  { counts[static_cast<uint8_t>('L')]++; }
    void handle(const MWCBDeclineLevel&)           { counts[static_cast<uint8_t>('V')]++; }
    void handle(const MWCBStatus&)                 { counts[static_cast<uint8_t>('W')]++; }
    void handle(const IPOQuotingPeriodUpdate&)      { counts[static_cast<uint8_t>('K')]++; }
    void handle(const LULDAuctionCollar&)           { counts[static_cast<uint8_t>('J')]++; }
    void handle(const OperationalHalt&)             { counts[static_cast<uint8_t>('h')]++; }
    void handle(const AddOrder&)                    { counts[static_cast<uint8_t>('A')]++; }
    void handle(const AddOrderMPIDAttribution&)     { counts[static_cast<uint8_t>('F')]++; }
    void handle(const OrderExecuted&)               { counts[static_cast<uint8_t>('E')]++; }
    void handle(const OrderExecutedWithPrice&)      { counts[static_cast<uint8_t>('C')]++; }
    void handle(const OrderCancel&)                 { counts[static_cast<uint8_t>('X')]++; }
    void handle(const OrderDelete&)                 { counts[static_cast<uint8_t>('D')]++; }
    void handle(const OrderReplace&)                { counts[static_cast<uint8_t>('U')]++; }
    void handle(const TradeNonCross&)               { counts[static_cast<uint8_t>('P')]++; }
    void handle(const CrossTrade&)                  { counts[static_cast<uint8_t>('Q')]++; }
    void handle(const BrokenTrade&)                 { counts[static_cast<uint8_t>('B')]++; }
    void handle(const NOII&)                        { counts[static_cast<uint8_t>('I')]++; }
    void handle(const RetailInterest&)              { counts[static_cast<uint8_t>('N')]++; }
};

TEST(DispatchTests, FullHandler_DispatchTable_AllTypes) {
    FullHandler handler;
    DispatchTable<FullHandler> table;

    uint8_t buf[50] = {0};

    for (char type : ALL_MESSAGE_TYPES) {
        buf[0] = type;
        EXPECT_TRUE(table.dispatch(handler, type, reinterpret_cast<char*>(buf)))
            << "DispatchTable returned false for type '" << type << "'";
    }

    // Verify each type dispatched exactly once
    for (char type : ALL_MESSAGE_TYPES) {
        EXPECT_EQ(handler.counts[static_cast<uint8_t>(type)], 1)
            << "Type '" << type << "' not dispatched exactly once";
    }

    // Unknown type returns false
    EXPECT_FALSE(table.dispatch(handler, 'Z', reinterpret_cast<char*>(buf)));
}

TEST(DispatchTests, FullHandler_StaticDispatch_AllTypes) {
    FullHandler handler;

    uint8_t buf[50] = {0};

    for (char type : ALL_MESSAGE_TYPES) {
        buf[0] = type;
        EXPECT_TRUE(dispatch_message(handler, type, reinterpret_cast<char*>(buf)));
    }

    for (char type : ALL_MESSAGE_TYPES) {
        EXPECT_EQ(handler.counts[static_cast<uint8_t>(type)], 1)
            << "Type '" << type << "' not dispatched exactly once via static dispatch";
    }
}

// =============================================================================
// STREAM PARSER TESTS
// =============================================================================

TEST(StreamTests, ParseStream_TwoMessages) {
    MockHandler handler;

    // Construct a MoldUDP64 stream with an AddOrder and an OrderDelete
    uint8_t stream[128] = {0};
    std::size_t offset = 0;

    // Msg 1: AddOrder (36 bytes)
    write_be16(stream + offset, 36);
    stream[offset + 2] = 'A';
    std::memcpy(stream + offset + 2 + 24, "MSFT    ", 8);
    offset += 2 + 36;

    // Msg 2: OrderDelete (19 bytes)
    write_be16(stream + offset, 19);
    stream[offset + 2] = 'D';
    write_be64(stream + offset + 2 + 11, 42);
    offset += 2 + 19;

    std::size_t processed = parse_stream(handler, reinterpret_cast<char*>(stream), offset);

    EXPECT_EQ(processed, offset);
    EXPECT_EQ(handler.add_orders, 1);
    EXPECT_EQ(handler.deletes, 1);
}

TEST(StreamTests, ParseStream_FullHandler_AllTypes) {
    FullHandler handler;

    // Build a stream containing one of each message type
    uint8_t stream[2048] = {0};
    std::size_t offset = 0;

    for (char type : ALL_MESSAGE_TYPES) {
        std::size_t sz = message_size(type);
        ASSERT_GT(sz, 0u) << "Unknown message size for type '" << type << "'";

        write_be16(stream + offset, static_cast<uint16_t>(sz));
        stream[offset + 2] = type;
        offset += 2 + sz;
    }

    std::size_t processed = parse_stream(handler, reinterpret_cast<char*>(stream), offset);

    EXPECT_EQ(processed, offset);

    for (char type : ALL_MESSAGE_TYPES) {
        EXPECT_EQ(handler.counts[static_cast<uint8_t>(type)], 1)
            << "Stream parse didn't dispatch type '" << type << "'";
    }
}

TEST(StreamTests, ParseStream_IncompleteMessage_StopsEarly) {
    FullHandler handler;

    uint8_t stream[64] = {0};
    std::size_t offset = 0;

    // Complete AddOrder
    write_be16(stream + offset, 36);
    stream[offset + 2] = 'A';
    offset += 2 + 36;

    // Incomplete OrderDelete (claim 19 bytes but only provide 5)
    write_be16(stream + offset, 19);
    stream[offset + 2] = 'D';
    std::size_t total = offset + 2 + 5;  // not enough data

    std::size_t processed = parse_stream(handler, reinterpret_cast<char*>(stream), total);

    // Should only process the first complete message
    EXPECT_EQ(processed, offset);
    EXPECT_EQ(handler.counts[static_cast<uint8_t>('A')], 1);
    EXPECT_EQ(handler.counts[static_cast<uint8_t>('D')], 0);
}

TEST(StreamTests, ParseStream_EmptyBuffer) {
    FullHandler handler;

    std::size_t processed = parse_stream(handler, nullptr, 0);
    EXPECT_EQ(processed, 0u);
}

TEST(StreamTests, ParseStream_OnlyLengthPrefix_NoMessageBody) {
    FullHandler handler;

    // Buffer with just a length prefix (2 bytes) but the claimed length extends
    // beyond the buffer
    uint8_t stream[2] = {0};
    write_be16(stream, 36);  // claims 36 bytes follow

    std::size_t processed = parse_stream(handler, reinterpret_cast<char*>(stream), 2);
    EXPECT_EQ(processed, 0u);
}

TEST(StreamTests, ParseStream_ZeroLengthRecordStopsSafely) {
    FullHandler handler;
    const uint8_t stream[2] = {0, 0};

    const std::size_t processed = parse_stream(
        handler, reinterpret_cast<const char*>(stream), sizeof(stream));

    EXPECT_EQ(processed, 0u);
    for (char type : ALL_MESSAGE_TYPES) {
        EXPECT_EQ(handler.counts[static_cast<uint8_t>(type)], 0);
    }
}
