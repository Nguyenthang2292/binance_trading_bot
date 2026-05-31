#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class SigningMethod { HMAC_SHA256, Ed25519 };

class Signer {
public:
    /** Creates a signer for Binance signed endpoints (HMAC-SHA256 or Ed25519). */
    explicit Signer(std::string secretKey, SigningMethod method = SigningMethod::HMAC_SHA256);
    ~Signer();
    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;
    Signer(Signer&&) = delete;
    Signer& operator=(Signer&&) = delete;

    /** Returns the signature for a canonical request payload string. */
    std::string sign(std::string_view payload) const;
    /** Ensures timestamp presence and appends a recomputed signature parameter. */
    std::string addSignature(std::string_view params) const;

    SigningMethod method() const { return m_method; }

private:
    std::vector<unsigned char> m_secretKey;
    SigningMethod m_method;

    static int64_t nowMs();
    std::string_view secretKeyView() const;
    std::string signHmacSha256(std::string_view payload) const;
    std::string signEd25519(std::string_view payload) const;
};
