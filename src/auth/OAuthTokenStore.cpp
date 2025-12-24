#include "auth/OAuthTokenStore.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

bool OAuthTokenStore::LoadFromFile(const std::string& path, TokenInfo* out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Token file not found: " << path << "\n";
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse token file: " << ex.what() << "\n";
        return false;
    }

    out->access_token = j.value("access_token", "");
    out->refresh_token = j.value("refresh_token", "");
    out->expiry_ts = j.value("expiry_ts", 0);
    out->token_type = j.value("token_type", "Bearer");
    return true;
}

bool OAuthTokenStore::SaveToFile(const std::string& path, const TokenInfo& info) {
    nlohmann::json j;
    j["access_token"] = info.access_token;
    j["refresh_token"] = info.refresh_token;
    j["expiry_ts"] = info.expiry_ts;
    j["token_type"] = info.token_type;

    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open token file for write: " << path << "\n";
        return false;
    }
    file << j.dump(2);
    file.close();

#ifndef _WIN32
    if (chmod(path.c_str(), 0600) != 0) {
        std::cerr << "Warning: failed to set token file permissions on " << path << "\n";
    }
#endif
    return true;
}
