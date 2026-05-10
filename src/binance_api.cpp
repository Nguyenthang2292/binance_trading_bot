#include "binance_api.h"
#include "logger.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <algorithm>

BinanceAPI::BinanceAPI(const std::string& apiKey, const std::string& secretKey)
    : m_apiKey(apiKey), m_secretKey(secretKey)
    , m_baseUrl("https://api.binance.com")
    , m_curl(nullptr)
{
    curl_global_init(CURL_GLOBAL_ALL);
    m_curl = curl_easy_init();
    Logger::instance().log(LogLevel::INFO, "BinanceAPI initialized");
}

BinanceAPI::~BinanceAPI() {
    if (m_curl) {
        curl_easy_cleanup(m_curl);
    }
    curl_global_cleanup();
}

size_t BinanceAPI::writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    auto* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::string BinanceAPI::buildQueryString(const std::map<std::string, std::string>& params) const {
    std::string result;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (it != params.begin()) result += "&";
        char* encoded = curl_easy_escape(m_curl, it->first.c_str(), it->first.length());
        result += std::string(encoded);
        curl_free(encoded);
        result += "=";
        encoded = curl_easy_escape(m_curl, it->second.c_str(), it->second.length());
        result += std::string(encoded);
        curl_free(encoded);
    }
    return result;
}

std::string BinanceAPI::signRequest(const std::string& queryString) const {
    unsigned char* digest = HMAC(EVP_sha256(),
        m_secretKey.c_str(), m_secretKey.length(),
        reinterpret_cast<const unsigned char*>(queryString.c_str()), queryString.length(),
        nullptr, nullptr);

    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << (int)digest[i];
    }
    return ss.str();
}

std::string BinanceAPI::makeRequest(const std::string& endpoint, const std::string& method,
                                     const std::map<std::string, std::string>& params, bool signed_) {
    if (!m_curl) {
        Logger::instance().log(LogLevel::ERROR, "CURL not initialized");
        return "";
    }

    std::map<std::string, std::string> finalParams = params;
    if (signed_) {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        finalParams["timestamp"] = std::to_string(ms);
    }

    std::string queryString = buildQueryString(finalParams);
    if (signed_) {
        std::string signature = signRequest(queryString);
        queryString += "&signature=" + signature;
    }

    std::string url = m_baseUrl + endpoint;
    if (!queryString.empty() && method == "GET") {
        url += "?" + queryString;
    }

    std::string responseStr;
    curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &responseStr);
    curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, 2L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + m_apiKey).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, headers);

    if (method == "POST") {
        curl_easy_setopt(m_curl, CURLOPT_POST, 1L);
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, queryString.c_str());
    } else if (method == "DELETE") {
        curl_easy_setopt(m_curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, queryString.c_str());
    }

    CURLcode res = curl_easy_perform(m_curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        Logger::instance().log(LogLevel::ERROR, "CURL error: " + std::string(curl_easy_strerror(res)));
        return "";
    }

    long httpCode = 0;
    curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if (httpCode >= 400) {
        Logger::instance().log(LogLevel::ERROR, "HTTP error " + std::to_string(httpCode) + ": " + responseStr);
        return "";
    }

    return responseStr;
}

std::optional<double> BinanceAPI::getPrice(const std::string& symbol) {
    std::string response = makeRequest("/api/v3/ticker/price",
        "GET", {{"symbol", symbol}}, false);
    if (response.empty()) return std::nullopt;

    try {
        json j = json::parse(response);
        return std::stod(j["price"].get<std::string>());
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::ERROR, "Failed to parse price: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::vector<Kline> BinanceAPI::getKlines(const std::string& symbol, const std::string& interval, int limit) {
    auto params = std::map<std::string, std::string>{
        {"symbol", symbol}, {"interval", interval}, {"limit", std::to_string(limit)}
    };
    std::string response = makeRequest("/api/v3/klines", "GET", params, false);

    std::vector<Kline> klines;
    if (response.empty()) return klines;

    try {
        json j = json::parse(response);
        for (const auto& k : j) {
            Kline kl;
            kl.openTime = k[0].get<long long>();
            kl.open = std::stod(k[1].get<std::string>());
            kl.high = std::stod(k[2].get<std::string>());
            kl.low = std::stod(k[3].get<std::string>());
            kl.close = std::stod(k[4].get<std::string>());
            kl.volume = std::stod(k[5].get<std::string>());
            kl.closeTime = k[6].get<long long>();
            kl.quoteAssetVolume = std::stod(k[7].get<std::string>());
            kl.numberOfTrades = k[8].get<int>();
            klines.push_back(kl);
        }
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::ERROR, "Failed to parse klines: " + std::string(e.what()));
    }
    return klines;
}

std::vector<Ticker> BinanceAPI::get24hrTickers() {
    std::string response = makeRequest("/api/v3/ticker/24hr", "GET", {}, false);
    std::vector<Ticker> tickers;
    if (response.empty()) return tickers;

    try {
        json j = json::parse(response);
        for (const auto& t : j) {
            Ticker tk;
            tk.symbol = t["symbol"].get<std::string>();
            tk.price = std::stod(t["lastPrice"].get<std::string>());
            tk.priceChange = std::stod(t["priceChange"].get<std::string>());
            tk.priceChangePercent = std::stod(t["priceChangePercent"].get<std::string>());
            tk.highPrice = std::stod(t["highPrice"].get<std::string>());
            tk.lowPrice = std::stod(t["lowPrice"].get<std::string>());
            tk.volume = std::stod(t["volume"].get<std::string>());
            tickers.push_back(tk);
        }
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::ERROR, "Failed to parse tickers: " + std::string(e.what()));
    }
    return tickers;
}

std::optional<AccountInfo> BinanceAPI::getAccountInfo() {
    std::string response = makeRequest("/api/v3/account", "GET", {}, true);
    if (response.empty()) return std::nullopt;

    try {
        json j = json::parse(response);
        AccountInfo info;
        info.totalBalance = 0.0;
        for (const auto& b : j["balances"]) {
            double free = std::stod(b["free"].get<std::string>());
            double locked = std::stod(b["locked"].get<std::string>());
            double total = free + locked;
            if (total > 0.0) {
                info.balances[b["asset"].get<std::string>()] = total;
                info.totalBalance += total;
            }
        }
        return info;
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::ERROR, "Failed to parse account: " + std::string(e.what()));
        return std::nullopt;
    }
}

bool BinanceAPI::testConnectivity() {
    std::string response = makeRequest("/api/v3/ping", "GET", {}, false);
    return !response.empty();
}

OrderResult BinanceAPI::createOrder(const std::string& symbol, const std::string& side,
                                     const std::string& type, double quantity,
                                     std::optional<double> price) {
    std::map<std::string, std::string> params = {
        {"symbol", symbol}, {"side", side},
        {"type", type}, {"quantity", std::to_string(quantity)}
    };

    if (price.has_value()) {
        params["price"] = std::to_string(price.value());
        params["timeInForce"] = "GTC";
    }

    std::string response = makeRequest("/api/v3/order", "POST", params, true);
    OrderResult result;
    result.status = "FAILED";
    if (response.empty()) return result;

    try {
        json j = json::parse(response);
        result.symbol = j["symbol"].get<std::string>();
        result.orderId = j["orderId"].get<long long>();
        result.status = j["status"].get<std::string>();
        result.price = std::stod(j["price"].get<std::string>());
        result.origQty = std::stod(j["origQty"].get<std::string>());
        result.executedQty = std::stod(j["executedQty"].get<std::string>());
        result.side = j["side"].get<std::string>();
        result.type = j["type"].get<std::string>();
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::ERROR, "Failed to parse order: " + std::string(e.what()));
        result.status = "PARSE_ERROR";
    }
    return result;
}

bool BinanceAPI::cancelOrder(const std::string& symbol, long long orderId) {
    auto params = std::map<std::string, std::string>{
        {"symbol", symbol}, {"orderId", std::to_string(orderId)}
    };
    std::string response = makeRequest("/api/v3/order", "DELETE", params, true);
    return !response.empty();
}

std::optional<OrderResult> BinanceAPI::getOrder(const std::string& symbol, long long orderId) {
    auto params = std::map<std::string, std::string>{
        {"symbol", symbol}, {"orderId", std::to_string(orderId)}
    };
    std::string response = makeRequest("/api/v3/order", "GET", params, true);
    if (response.empty()) return std::nullopt;

    try {
        json j = json::parse(response);
        OrderResult result;
        result.symbol = j["symbol"].get<std::string>();
        result.orderId = j["orderId"].get<long long>();
        result.status = j["status"].get<std::string>();
        result.price = std::stod(j["price"].get<std::string>());
        result.origQty = std::stod(j["origQty"].get<std::string>());
        result.executedQty = std::stod(j["executedQty"].get<std::string>());
        result.side = j["side"].get<std::string>();
        result.type = j["type"].get<std::string>();
        return result;
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::ERROR, "Failed to parse order: " + std::string(e.what()));
        return std::nullopt;
    }
}
