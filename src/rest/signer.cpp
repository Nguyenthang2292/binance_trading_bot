#include "rest/signer.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

std::string toHex(const unsigned char* data, size_t len) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        out << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return out.str();
}

std::vector<unsigned char> hexToBytes(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::invalid_argument("Ed25519 private key hex length must be even");
    }

    std::vector<unsigned char> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned int value = 0;
        std::istringstream in(std::string(hex.substr(i, 2)));
        in >> std::hex >> value;
        if (!in) {
            throw std::invalid_argument("invalid hex private key");
        }
        bytes.push_back(static_cast<unsigned char>(value));
    }
    return bytes;
}

std::string toBase64(const unsigned char* data, size_t len) {
    if (len == 0) {
        return {};
    }
    const auto outLen = static_cast<size_t>(4 * ((len + 2) / 3));
    std::string out(outLen, '\0');
    const int encodedLen = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()),
        data,
        static_cast<int>(len));
    if (encodedLen < 0) {
        throw std::runtime_error("failed to base64 encode Ed25519 signature");
    }
    out.resize(static_cast<size_t>(encodedLen));
    return out;
}

bool hasQueryParam(std::string_view params, std::string_view key) {
    size_t pos = 0;
    while (pos <= params.size()) {
        const auto next = params.find('&', pos);
        const auto end = next == std::string_view::npos ? params.size() : next;
        const auto token = params.substr(pos, end - pos);
        const auto eq = token.find('=');
        const auto tokenKey = eq == std::string_view::npos ? token : token.substr(0, eq);
        if (tokenKey == key) {
            return true;
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }
    return false;
}

void removeQueryParam(std::string& params, std::string_view key) {
    std::string out;
    size_t pos = 0;
    while (pos <= params.size()) {
        const auto next = params.find('&', pos);
        const auto end = next == std::string::npos ? params.size() : next;
        const auto token = std::string_view(params).substr(pos, end - pos);
        const auto eq = token.find('=');
        const auto tokenKey = eq == std::string_view::npos ? token : token.substr(0, eq);
        if (tokenKey != key) {
            if (!out.empty()) {
                out.push_back('&');
            }
            out.append(token.data(), token.size());
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    params = std::move(out);
}

} // namespace

Signer::Signer(std::string secretKey, SigningMethod method)
    : m_secretKey(std::move(secretKey)), m_method(method) {}

std::string Signer::sign(std::string_view payload) const {
    if (m_method == SigningMethod::HMAC_SHA256) {
        return signHmacSha256(payload);
    }
    return signEd25519(payload);
}

std::string Signer::addSignature(std::string_view params) const {
    std::string signedParams(params);
    removeQueryParam(signedParams, "signature");

    if (!hasQueryParam(signedParams, "timestamp")) {
        if (!signedParams.empty()) {
            signedParams += '&';
        }
        signedParams += "timestamp=" + std::to_string(nowMs());
    }

    signedParams += "&signature=" + sign(signedParams);
    return signedParams;
}

int64_t Signer::nowMs() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

std::string Signer::signHmacSha256(std::string_view payload) const {
    unsigned int len = SHA256_DIGEST_LENGTH;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    HMAC(EVP_sha256(),
         m_secretKey.data(),
         static_cast<int>(m_secretKey.size()),
         reinterpret_cast<const unsigned char*>(payload.data()),
         payload.size(),
         digest,
         &len);
    return toHex(digest, len);
}

std::string Signer::signEd25519(std::string_view payload) const {
    auto keyBytes = hexToBytes(m_secretKey);
    if (keyBytes.size() != 32 && keyBytes.size() != 64) {
        throw std::invalid_argument("Ed25519 private key must be 32 or 64 bytes in hex");
    }

    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, keyBytes.data(), keyBytes.size() == 64 ? 32 : keyBytes.size());
    if (!pkey) {
        throw std::runtime_error("failed to create Ed25519 private key");
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("failed to allocate Ed25519 signing context");
    }

    size_t sigLen = 0;
    if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) != 1 ||
        EVP_DigestSign(ctx, nullptr, &sigLen,
                       reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("failed to size Ed25519 signature");
    }

    std::vector<unsigned char> sig(sigLen);
    if (EVP_DigestSign(ctx, sig.data(), &sigLen,
                       reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("failed to sign Ed25519 payload");
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return toBase64(sig.data(), sigLen);
}
