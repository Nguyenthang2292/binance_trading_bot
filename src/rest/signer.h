#pragma once

#include <cstdint>
#include <string>
#include <string_view>

enum class SigningMethod { HMAC_SHA256, Ed25519 };

class Signer {
public:
    explicit Signer(std::string secretKey, SigningMethod method = SigningMethod::HMAC_SHA256);

    std::string sign(std::string_view payload) const;
    std::string addSignature(std::string_view params) const;

    SigningMethod method() const { return m_method; }

private:
    std::string m_secretKey;
    SigningMethod m_method;

    static int64_t nowMs();
    std::string signHmacSha256(std::string_view payload) const;
    std::string signEd25519(std::string_view payload) const;
};
