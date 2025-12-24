#pragma once

#include <cstdint>
#include <string>

struct TokenInfo {
    std::string access_token;
    std::string refresh_token;
    int64_t expiry_ts = 0;
    std::string token_type;
};

class OAuthTokenStore {
public:
    static bool LoadFromFile(const std::string& path, TokenInfo* out);
    static bool SaveToFile(const std::string& path, const TokenInfo& info);
};
