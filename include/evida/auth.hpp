#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace evida::auth {

struct AuthSmtpConfig {
    std::string smtp_host = "smtp.gmail.com";
    int smtp_port = 587;
    std::string smtp_user;
    std::string smtp_pass;
    std::string smtp_from;
    std::string smtp_from_name = "EVIDA SisTer";
    std::string auth_secret;
    int code_ttl_seconds = 600;      // 10 minutes
    int cooldown_seconds = 60;       // 60 seconds between resends
    int max_attempts = 5;            // Max 5 attempts per code
    bool simulated = false;          // For automated testing
    bool dev_mode = true;            // Enabled by default in local dev, shows dev_code

    static AuthSmtpConfig from_environment();
};

struct UserRecord {
    std::string id;
    std::string email;
    std::string name;
    std::string role; // "admin", "researcher", "user"
    bool is_active = true;
    std::string created_at;
    std::string last_login_at;
};

struct RequestCodeResult {
    bool ok = false;
    int status_code = 200;
    std::string user_message;
    std::string error_detail;
    std::string dev_code;
    int retry_after_seconds = 0;
};

struct VerifyCodeResult {
    bool ok = false;
    int status_code = 200;
    std::string user_message;
    std::string error_detail;
    std::string session_token;
    UserRecord user;
    int remaining_attempts = 0;
};

// Cryptographic primitives
std::string generate_numeric_code(std::size_t digits = 6);
std::string compute_hmac_sha256(std::string_view data, std::string_view key);
std::string sha256_hex(std::string_view data);
std::string generate_session_token();
bool constant_time_compare(std::string_view a, std::string_view b);
std::string normalize_email(std::string_view email);

// Rate limiter
class RateLimiter {
public:
    RateLimiter() = default;
    int check_and_record_request(const std::string& email, const std::string& ip, int cooldown_sec = 60);
    void reset();

private:
    std::mutex mutex_;
    struct EmailHistory {
        std::chrono::steady_clock::time_point last_request;
        std::vector<std::chrono::steady_clock::time_point> window_requests;
    };
    std::map<std::string, EmailHistory> email_histories_;
    std::map<std::string, std::vector<std::chrono::steady_clock::time_point>> ip_histories_;
};

// Core Authentication & User Management (Padrão MORFO)
class AuthSmtpService {
public:
    explicit AuthSmtpService(AuthSmtpConfig config);

    const AuthSmtpConfig& config() const { return config_; }
    void set_config(AuthSmtpConfig config) { config_ = std::move(config); }

    // Request a 6-digit code sent via SMTP (or previewed in dev_mode)
    RequestCodeResult request_code(sqlite3* db, const std::string& email, const std::string& ip_address);

    // Verify 6-digit code and issue session token
    VerifyCodeResult verify_code(sqlite3* db, const std::string& email, const std::string& code, const std::string& ip_address);

    // Session validation
    std::optional<UserRecord> validate_session(sqlite3* db, const std::string& session_token);

    // Logout
    bool revoke_session(sqlite3* db, const std::string& session_token);

    // User management
    bool add_user(sqlite3* db, const std::string& email, const std::string& name, const std::string& role);
    std::vector<UserRecord> list_users(sqlite3* db);
    bool set_user_active(sqlite3* db, const std::string& email, bool active);
    std::optional<UserRecord> find_user_by_email(sqlite3* db, const std::string& email);

    // Audit logging
    void record_audit(sqlite3* db, const std::string& email, const std::string& ip,
                      const std::string& action, const std::string& decision, const std::string& details);

private:
    AuthSmtpConfig config_;
    RateLimiter rate_limiter_;

    bool send_email(const std::string& to_email, const std::string& code, std::string& err_out);
};

} // namespace evida::auth
