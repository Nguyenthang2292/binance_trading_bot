#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct Kline {
    long long openTime;
    double open;
    double high;
    double low;
    double close;
    double volume;
    long long closeTime;
    double quoteAssetVolume;
    int numberOfTrades;
};

struct Ticker {
    std::string symbol;
    double price;
    double priceChange;
    double priceChangePercent;
    double highPrice;
    double lowPrice;
    double volume;
};

struct AccountInfo {
    double totalBalance;
    std::map<std::string, double> balances;
};

struct OrderResult {
    std::string symbol;
    long long orderId;
    std::string status;
    double price;
    double origQty;
    double executedQty;
    std::string side;
    std::string type;
};

class BinanceAPI {
public:
    BinanceAPI(const std::string& apiKey, const std::string& secretKey);
    ~BinanceAPI();

    std::optional<double> getPrice(const std::string& symbol);
    std::vector<Kline> getKlines(const std::string& symbol, const std::string& interval, int limit = 100);
    std::vector<Ticker> get24hrTickers();
    std::optional<AccountInfo> getAccountInfo();
    bool testConnectivity();

    OrderResult createOrder(const std::string& symbol, const std::string& side,
                            const std::string& type, double quantity, std::optional<double> price = std::nullopt);
    bool cancelOrder(const std::string& symbol, long long orderId);
    std::optional<OrderResult> getOrder(const std::string& symbol, long long orderId);

private:
    std::string m_apiKey;
    std::string m_secretKey;
    std::string m_baseUrl;
    CURL* m_curl;

    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
    std::string signRequest(const std::string& queryString) const;
    std::string makeRequest(const std::string& endpoint, const std::string& method = "GET",
                            const std::map<std::string, std::string>& params = {}, bool signed_ = false);
    std::string buildQueryString(const std::map<std::string, std::string>& params) const;
};
