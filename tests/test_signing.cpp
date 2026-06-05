#include <gtest/gtest.h>

#include "rest/signer.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<Signer>);
static_assert(!std::is_move_constructible_v<Signer>);

TEST(SignerTest, HmacSha256KnownVector) {
    Signer signer("key");
    EXPECT_EQ(
        signer.sign("The quick brown fox jumps over the lazy dog"),
        "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
}

TEST(SignerTest, AddSignatureAppendsTimestampAndSignatureLast) {
    Signer signer("secret");
    const auto signedParams = signer.addSignature("symbol=BTCUSDT&side=BUY");

    EXPECT_NE(signedParams.find("symbol=BTCUSDT&side=BUY&timestamp="), std::string::npos);
    const auto sigPos = signedParams.find("&signature=");
    ASSERT_NE(sigPos, std::string::npos);
    EXPECT_EQ(signedParams.rfind("&signature="), sigPos);
    EXPECT_GT(signedParams.size(), sigPos + std::string("&signature=").size());
}

TEST(SignerTest, AddSignatureBiasesGeneratedTimestampBackward) {
    Signer signer("secret");
    const auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    const auto signedParams = signer.addSignature("symbol=BTCUSDT");

    const auto timestampPos = signedParams.find("timestamp=");
    ASSERT_NE(timestampPos, std::string::npos);
    const auto valueStart = timestampPos + std::string("timestamp=").size();
    const auto valueEnd = signedParams.find('&', valueStart);
    ASSERT_NE(valueEnd, std::string::npos);
    const auto timestamp = std::stoll(signedParams.substr(valueStart, valueEnd - valueStart));
    EXPECT_LT(timestamp, before);
    EXPECT_GT(timestamp, before - 5000);
}

TEST(SignerTest, AddSignatureAppliesServerTimeOffset) {
    Signer signer("secret");
    signer.setTimeOffsetMs(-10'000);
    const auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    const auto signedParams = signer.addSignature("symbol=BTCUSDT");

    const auto timestampPos = signedParams.find("timestamp=");
    ASSERT_NE(timestampPos, std::string::npos);
    const auto valueStart = timestampPos + std::string("timestamp=").size();
    const auto valueEnd = signedParams.find('&', valueStart);
    ASSERT_NE(valueEnd, std::string::npos);
    const auto timestamp = std::stoll(signedParams.substr(valueStart, valueEnd - valueStart));
    EXPECT_LT(timestamp, before - 9'000);
    EXPECT_GT(timestamp, before - 15'000);
}

TEST(SignerTest, AddSignatureUsesProvidedTimestampWithoutAddingDuplicate) {
    Signer signer("secret");
    const auto signedParams = signer.addSignature("symbol=BTCUSDT&timestamp=1700000000000");

    EXPECT_EQ(signedParams.find("timestamp=1700000000000"), signedParams.rfind("timestamp=1700000000000"));
    EXPECT_NE(signedParams.find("&signature="), std::string::npos);
}

TEST(SignerTest, AddSignatureReturnsProvidedSignatureUnchanged) {
    Signer signer("secret");
    const std::string params = "symbol=BTCUSDT&timestamp=1700000000000&signature=abc123";

    const auto signedParams = signer.addSignature(params);
    EXPECT_NE(signedParams, params);
    EXPECT_NE(signedParams.find("timestamp=1700000000000"), std::string::npos);
    EXPECT_NE(signedParams.find("&signature="), std::string::npos);
    EXPECT_EQ(signedParams.find("signature=abc123"), std::string::npos);
}

TEST(SignerTest, Ed25519UsesBase64Encoding) {
    const std::string privateKeyHex = "1f1e1d1c1b1a19181716151413121110"
                                      "0f0e0d0c0b0a09080706050403020100";
    Signer signer(privateKeyHex, SigningMethod::Ed25519);

    const auto signature = signer.sign("symbol=BTCUSDT&side=BUY");

    EXPECT_EQ(signature.size(), 88u);
    EXPECT_NE(signature.find('='), std::string::npos);
}
