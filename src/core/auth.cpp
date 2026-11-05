// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 EVIDA Contributors
// This file is part of EVIDA - Scientific Evidence Intelligence System
// Licensed under the GNU General Public License v3.0 or later.

#include "evida/auth.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <sqlite3.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace fs = std::filesystem;

namespace evida::auth {

namespace {

std::string utc_now() {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string utc_future(int seconds) {
    auto now = std::chrono::system_clock::now() + std::chrono::seconds(seconds);
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string safe_col_text(sqlite3_stmt* stmt, int col) {
    const auto* txt = sqlite3_column_text(stmt, col);
    return txt ? reinterpret_cast<const char*>(txt) : "";
}

} // namespace

std::string normalize_email(std::string_view email) {
    std::string out;
    out.reserve(email.size());
    for (char c : email) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

std::string sha256_hex(std::string_view data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char c : hash) {
        ss << std::setw(2) << static_cast<int>(c);
    }
    return ss.str();
}

std::string compute_hmac_sha256(std::string_view data, std::string_view key) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         md, &md_len);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < md_len; ++i) {
        ss << std::setw(2) << static_cast<int>(md[i]);
    }
    return ss.str();
}

std::string generate_numeric_code(std::size_t digits) {
    std::string code;
    code.resize(digits);
    unsigned char buf[32];
    if (RAND_bytes(buf, static_cast<int>(digits)) != 1) {
        throw std::runtime_error("OpenSSL RAND_bytes failed");
    }
    for (std::size_t i = 0; i < digits; ++i) {
        code[i] = '0' + (buf[i] % 10);
    }
    return code;
}

std::string generate_session_token() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("OpenSSL RAND_bytes failed");
    }
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char b : bytes) {
        ss << std::setw(2) << static_cast<int>(b);
    }
    return ss.str();
}

bool constant_time_compare(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

int RateLimiter::check_and_record_request(const std::string& email, const std::string& ip, int cooldown_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    auto& eh = email_histories_[email];
    if (eh.last_request.time_since_epoch().count() > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - eh.last_request).count();
        if (elapsed < cooldown_sec) {
            return static_cast<int>(cooldown_sec - elapsed);
        }
    }

    if (!ip.empty()) {
        auto& iph = ip_histories_[ip];
        auto cutoff = now - std::chrono::hours(1);
        iph.erase(std::remove_if(iph.begin(), iph.end(), [&](const auto& t) { return t < cutoff; }), iph.end());
        if (iph.size() >= 30) {
            return 300;
        }
        iph.push_back(now);
    }

    eh.last_request = now;
    return 0;
}

void RateLimiter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    email_histories_.clear();
    ip_histories_.clear();
}

AuthSmtpConfig AuthSmtpConfig::from_environment() {
    AuthSmtpConfig cfg;
    if (const char* h = std::getenv("EVIDA_SMTP_HOST")) cfg.smtp_host = h;
    if (const char* p = std::getenv("EVIDA_SMTP_PORT")) {
        try { cfg.smtp_port = std::stoi(p); } catch (...) {}
    }
    if (const char* u = std::getenv("EVIDA_SMTP_USER")) cfg.smtp_user = u;
    if (const char* pass = std::getenv("EVIDA_SMTP_PASS")) cfg.smtp_pass = pass;
    if (const char* f = std::getenv("EVIDA_SMTP_FROM")) cfg.smtp_from = f;
    if (const char* fn = std::getenv("EVIDA_SMTP_FROM_NAME")) cfg.smtp_from_name = fn;
    if (const char* sec = std::getenv("EVIDA_AUTH_SECRET")) cfg.auth_secret = sec;
    if (const char* sim = std::getenv("EVIDA_SMTP_SIMULATED")) {
        cfg.simulated = (std::string_view(sim) == "1" || std::string_view(sim) == "true");
    }
    if (const char* dev = std::getenv("EVIDA_AUTH_DEV_MODE")) {
        cfg.dev_mode = (std::string_view(dev) == "1" || std::string_view(dev) == "true");
    }
    if (cfg.auth_secret.empty()) {
        cfg.auth_secret = generate_session_token();
    }
    return cfg;
}

AuthSmtpService::AuthSmtpService(AuthSmtpConfig config)
    : config_(std::move(config)) {
    if (config_.auth_secret.empty()) {
        config_.auth_secret = generate_session_token();
    }
}

void AuthSmtpService::record_audit(sqlite3* db, const std::string& email, const std::string& ip,
                                  const std::string& action, const std::string& decision, const std::string& details) {
    if (!db) return;
    const char* sql = R"(
        INSERT INTO auth_audit_events(id, email, ip_address, action, decision, details, occurred_at)
        VALUES(?, ?, ?, ?, ?, ?, ?);
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        std::string id = "aud-" + generate_session_token().substr(0, 16);
        std::string now = utc_now();
        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, ip.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, action.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, decision.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, details.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::optional<UserRecord> AuthSmtpService::find_user_by_email(sqlite3* db, const std::string& email) {
    if (!db) return std::nullopt;
    const char* sql = "SELECT id, email, name, role, is_active, created_at, last_login_at FROM users WHERE email=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<UserRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        UserRecord u;
        u.id = safe_col_text(stmt, 0);
        u.email = safe_col_text(stmt, 1);
        u.name = safe_col_text(stmt, 2);
        u.role = safe_col_text(stmt, 3);
        u.is_active = (sqlite3_column_int(stmt, 4) != 0);
        u.created_at = safe_col_text(stmt, 5);
        u.last_login_at = safe_col_text(stmt, 6);
        result = u;
    }
    sqlite3_finalize(stmt);
    return result;
}

bool AuthSmtpService::add_user(sqlite3* db, const std::string& email, const std::string& name, const std::string& role) {
    if (!db) return false;
    std::string norm = normalize_email(email);
    if (norm.empty() || norm.find('@') == std::string::npos) return false;

    const char* sql = R"(
        INSERT INTO users(id, email, name, role, is_active, created_at, last_login_at)
        VALUES(?, ?, ?, ?, 1, ?, ?)
        ON CONFLICT(email) DO UPDATE SET name=excluded.name, role=excluded.role, is_active=1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    std::string id = "usr-" + generate_session_token().substr(0, 12);
    std::string now = utc_now();
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, norm.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, now.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<UserRecord> AuthSmtpService::list_users(sqlite3* db) {
    std::vector<UserRecord> list;
    if (!db) return list;
    const char* sql = "SELECT id, email, name, role, is_active, created_at, last_login_at FROM users ORDER BY created_at ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        UserRecord u;
        u.id = safe_col_text(stmt, 0);
        u.email = safe_col_text(stmt, 1);
        u.name = safe_col_text(stmt, 2);
        u.role = safe_col_text(stmt, 3);
        u.is_active = (sqlite3_column_int(stmt, 4) != 0);
        u.created_at = safe_col_text(stmt, 5);
        u.last_login_at = safe_col_text(stmt, 6);
        list.push_back(u);
    }
    sqlite3_finalize(stmt);
    return list;
}

bool AuthSmtpService::set_user_active(sqlite3* db, const std::string& email, bool active) {
    if (!db) return false;
    std::string norm = normalize_email(email);
    const char* sql = "UPDATE users SET is_active=? WHERE email=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, active ? 1 : 0);
    sqlite3_bind_text(stmt, 2, norm.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool AuthSmtpService::send_email(const std::string& to_email, const std::string& code, std::string& err_out) {
    if (config_.simulated || config_.dev_mode) {
        return true;
    }
    if (config_.smtp_user.empty() || config_.smtp_pass.empty() || config_.smtp_from.empty()) {
        err_out = "Configuração SMTP incompleta (EVIDA_SMTP_USER/FROM e PASS necessários).";
        std::cerr << "[EVIDA SMTP ERROR] " << err_out << "\n";
        return false;
    }

    std::ostringstream msg;
    msg << "From: \"" << config_.smtp_from_name << "\" <" << config_.smtp_from << ">\r\n"
        << "To: <" << to_email << ">\r\n"
        << "Subject: EVIDA - Codigo de Acesso: " << code << "\r\n"
        << "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        << "Ola!\r\n\r\n"
        << "Voce solicitou acesso ao EVIDA — Observatorio de Evidencias Cientificas (Ecossistema SisTer - LABECO).\r\n"
        << "Seu codigo temporario de acesso e:\r\n\r\n"
        << "    " << code << "\r\n\r\n"
        << "Este codigo e de uso unico, permite ate 5 tentativas e expira em 10 minutos.\r\n"
        << "Se voce nao solicitou este codigo, por favor desconsidere este e-mail.\r\n\r\n"
        << "--\r\nEVIDA · Ecossistema SisTer · LABECO\r\n";

    fs::path temp_mail = fs::temp_directory_path() / ("evida-mail-" + generate_session_token().substr(0, 12) + ".eml");
    {
        std::ofstream ofs(temp_mail, std::ios::binary);
        if (!ofs) {
            err_out = "Não foi possível criar arquivo temporário de e-mail";
            return false;
        }
        ofs << msg.str();
    }

    std::string clean_pass = config_.smtp_pass;
    clean_pass.erase(std::remove_if(clean_pass.begin(), clean_pass.end(), [](unsigned char c) { return std::isspace(c); }), clean_pass.end());

    std::ostringstream cmd;
    cmd << "curl --silent --show-error --connect-timeout 15 --max-time 30";
    cmd << " --url 'smtp://" << config_.smtp_host << ":" << config_.smtp_port << "'";
    cmd << " --mail-from " << shell_quote(config_.smtp_from);
    cmd << " --mail-rcpt " << shell_quote(to_email);
    cmd << " --upload-file " << shell_quote(temp_mail.string());

    if (config_.smtp_port == 587 || config_.smtp_port == 25) {
        cmd << " --ssl-reqd";
    } else if (config_.smtp_port == 465) {
        cmd << " --ssl";
    }

    if (!config_.smtp_user.empty()) {
        cmd << " --user " << shell_quote(config_.smtp_user + ":" + clean_pass);
    }
    cmd << " 2>&1";

    FILE* pipe = popen(cmd.str().c_str(), "r");
    std::string curl_output;
    if (pipe) {
        char buf[512];
        while (fgets(buf, sizeof(buf), pipe)) {
            curl_output += buf;
        }
        int status = pclose(pipe);
        std::error_code ec;
        fs::remove(temp_mail, ec);
        if (status != 0) {
            err_out = "Falha no envio via SMTP: " + curl_output + " (código " + std::to_string(status) + ")";
            std::cerr << "[EVIDA SMTP ERROR] " << err_out << "\n";
            return false;
        }
    } else {
        std::error_code ec;
        fs::remove(temp_mail, ec);
        err_out = "Não foi possível disparar processo de entrega SMTP";
        return false;
    }
    return true;
}

RequestCodeResult AuthSmtpService::request_code(sqlite3* db, const std::string& email, const std::string& ip_address) {
    RequestCodeResult res;
    std::string norm = normalize_email(email);

    if (norm.empty() || norm.find('@') == std::string::npos || norm.find('.') == std::string::npos) {
        res.ok = false;
        res.status_code = 400;
        res.user_message = "Por favor, informe um endereço de e-mail institucional válido.";
        return res;
    }

    int retry_after = rate_limiter_.check_and_record_request(norm, ip_address, config_.cooldown_seconds);
    if (retry_after > 0) {
        record_audit(db, norm, ip_address, "REQUEST_CODE", "BLOCKED_RATE_LIMIT", "Cooldown: " + std::to_string(retry_after) + "s");
        res.ok = false;
        res.status_code = 429;
        res.retry_after_seconds = retry_after;
        res.user_message = "Por favor, aguarde " + std::to_string(retry_after) + " segundos antes de solicitar um novo código.";
        return res;
    }

    auto user_opt = find_user_by_email(db, norm);
    if (!user_opt.has_value() || !user_opt->is_active) {
        if (config_.dev_mode && !user_opt.has_value()) {
            add_user(db, norm, norm.substr(0, norm.find('@')), "researcher");
            user_opt = find_user_by_email(db, norm);
        } else {
            if (!config_.dev_mode) {
                std::this_thread::sleep_for(std::chrono::milliseconds(90));
            }
            record_audit(db, norm, ip_address, "REQUEST_CODE", "IGNORED_UNAUTHORIZED", "User absent or inactive");
            res.ok = true;
            res.status_code = 200;
            res.user_message = "Se o endereço estiver cadastrado e autorizado, um código de verificação foi enviado para sua caixa de entrada.";
            return res;
        }
    }

    // Invalidate previous pending codes
    {
        const char* invalidate_sql = "UPDATE auth_codes SET used_at=? WHERE email=? AND used_at IS NULL;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, invalidate_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string now = utc_now();
            sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, norm.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    std::string code = generate_numeric_code(6);
    std::string code_hmac = compute_hmac_sha256(code, config_.auth_secret);
    std::string code_id = "code-" + generate_session_token().substr(0, 16);
    std::string now = utc_now();
    std::string expires_at = utc_future(config_.code_ttl_seconds);

    {
        const char* insert_sql = R"(
            INSERT INTO auth_codes(id, email, code, code_hash, code_hmac, attempts, max_attempts, expires_at, used_at, created_at)
            VALUES(?, ?, '[HMAC_PROTECTED]', ?, ?, 0, ?, ?, NULL, ?);
        )";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            res.ok = false;
            res.status_code = 500;
            res.user_message = "Erro interno ao registrar chave de autenticação.";
            return res;
        }
        sqlite3_bind_text(stmt, 1, code_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, norm.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, code_hmac.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, code_hmac.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, config_.max_attempts);
        sqlite3_bind_text(stmt, 6, expires_at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    std::string err;
    if (!send_email(norm, code, err)) {
        record_audit(db, norm, ip_address, "REQUEST_CODE", "ERROR_SMTP_DELIVERY", err);
        res.ok = false;
        res.status_code = 502;
        res.user_message = "Não foi possível enviar o código via SMTP: " + err;
        return res;
    }

    record_audit(db, norm, ip_address, "REQUEST_CODE", "SENT", "Code generated, TTL=" + std::to_string(config_.code_ttl_seconds) + "s");
    res.ok = true;
    res.status_code = 200;
    res.user_message = "Código enviado para " + norm + ". Válido por 10 minutos.";
    if (config_.dev_mode) {
        res.dev_code = code;
    }
    return res;
}

VerifyCodeResult AuthSmtpService::verify_code(sqlite3* db, const std::string& email, const std::string& code, const std::string& ip_address) {
    VerifyCodeResult res;
    std::string norm = normalize_email(email);

    std::string clean_code;
    for (char c : code) {
        if (std::isdigit(static_cast<unsigned char>(c))) clean_code.push_back(c);
    }

    if (clean_code.size() != 6) {
        res.ok = false;
        res.status_code = 400;
        res.user_message = "O código deve conter exatamente 6 dígitos numéricos.";
        return res;
    }

    const char* find_sql = R"(
        SELECT id, code_hmac, attempts, max_attempts, expires_at, used_at
        FROM auth_codes
        WHERE email=?
        ORDER BY created_at DESC
        LIMIT 1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, find_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        res.ok = false;
        res.status_code = 500;
        res.user_message = "Erro interno ao validar chave.";
        return res;
    }

    sqlite3_bind_text(stmt, 1, norm.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        record_audit(db, norm, ip_address, "VERIFY_CODE", "FAILED_NO_CODE", "No pending code found");
        res.ok = false;
        res.status_code = 400;
        res.user_message = "Nenhum código pendente para este e-mail. Solicite um novo código.";
        return res;
    }

    std::string code_id = safe_col_text(stmt, 0);
    std::string stored_hmac = safe_col_text(stmt, 1);
    int attempts = sqlite3_column_int(stmt, 2);
    int max_attempts = sqlite3_column_int(stmt, 3);
    std::string expires_at = safe_col_text(stmt, 4);
    std::string used_at = safe_col_text(stmt, 5);
    sqlite3_finalize(stmt);

    std::string now = utc_now();

    if (!used_at.empty()) {
        record_audit(db, norm, ip_address, "VERIFY_CODE", "FAILED_ALREADY_USED", "Code already consumed");
        res.ok = false;
        res.status_code = 400;
        res.user_message = "Este código já foi utilizado. Solicite um novo código.";
        return res;
    }

    if (now > expires_at) {
        const char* exp_sql = "UPDATE auth_codes SET used_at=? WHERE id=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, exp_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, code_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        record_audit(db, norm, ip_address, "VERIFY_CODE", "FAILED_EXPIRED", "Code expired at " + expires_at);
        res.ok = false;
        res.status_code = 400;
        res.user_message = "O código expirou. Solicite um novo código de acesso.";
        return res;
    }

    attempts++;
    if (attempts > max_attempts) {
        const char* block_sql = "UPDATE auth_codes SET attempts=?, used_at=? WHERE id=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, block_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s, 1, attempts);
            sqlite3_bind_text(s, 2, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, code_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        record_audit(db, norm, ip_address, "VERIFY_CODE", "BLOCKED_MAX_ATTEMPTS", "Attempts exceeded");
        res.ok = false;
        res.status_code = 403;
        res.remaining_attempts = 0;
        res.user_message = "Número máximo de tentativas excedido (5). Este código foi invalidado.";
        return res;
    }

    std::string expected_hmac = compute_hmac_sha256(clean_code, config_.auth_secret);
    if (!constant_time_compare(stored_hmac, expected_hmac)) {
        int remaining = max_attempts - attempts;
        if (remaining <= 0) {
            const char* block_sql = "UPDATE auth_codes SET attempts=?, used_at=? WHERE id=?;";
            sqlite3_stmt* s = nullptr;
            if (sqlite3_prepare_v2(db, block_sql, -1, &s, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(s, 1, attempts);
                sqlite3_bind_text(s, 2, now.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, code_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }
            record_audit(db, norm, ip_address, "VERIFY_CODE", "BLOCKED_MAX_ATTEMPTS", "Attempts exhausted");
            res.ok = false;
            res.status_code = 403;
            res.remaining_attempts = 0;
            res.user_message = "Número máximo de tentativas excedido (5). Este código foi invalidado.";
            return res;
        }

        const char* upd_sql = "UPDATE auth_codes SET attempts=? WHERE id=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, upd_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s, 1, attempts);
            sqlite3_bind_text(s, 2, code_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        record_audit(db, norm, ip_address, "VERIFY_CODE", "FAILED_MISMATCH", "Attempt " + std::to_string(attempts));
        res.ok = false;
        res.status_code = 400;
        res.remaining_attempts = remaining;
        res.user_message = "Código incorreto. Tentativas restantes: " + std::to_string(remaining);
        return res;
    }

    // Code is valid! Consume code
    {
        const char* use_sql = "UPDATE auth_codes SET attempts=?, used_at=? WHERE id=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, use_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s, 1, attempts);
            sqlite3_bind_text(s, 2, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, code_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }

    auto user_opt = find_user_by_email(db, norm);
    if (!user_opt.has_value() || !user_opt->is_active) {
        res.ok = false;
        res.status_code = 403;
        res.user_message = "Usuário inativo ou não autorizado.";
        return res;
    }

    // Update last_login_at
    {
        const char* up_user = "UPDATE users SET last_login_at=? WHERE id=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, up_user, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, user_opt->id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }

    // Create session token (valid for 7 days)
    std::string token = generate_session_token();
    std::string session_expires = utc_future(7 * 86400);
    {
        const char* sess_sql = "INSERT INTO sessions(token, user_id, email, created_at, expires_at) VALUES(?, ?, ?, ?, ?);";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, sess_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, user_opt->id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, norm.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, session_expires.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }

    record_audit(db, norm, ip_address, "LOGIN_SUCCESS", "GRANTED", "Session issued for " + user_opt->name);
    res.ok = true;
    res.status_code = 200;
    res.session_token = token;
    res.user = *user_opt;
    res.user_message = "Autenticação realizada com sucesso.";
    return res;
}

std::optional<UserRecord> AuthSmtpService::validate_session(sqlite3* db, const std::string& session_token) {
    if (!db || session_token.empty()) return std::nullopt;
    const char* sql = R"(
        SELECT u.id, u.email, u.name, u.role, u.is_active, u.created_at, u.last_login_at
        FROM sessions s
        JOIN users u ON u.id = s.user_id
        WHERE s.token = ? AND s.expires_at > ? AND u.is_active = 1
        LIMIT 1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    std::string now = utc_now();
    sqlite3_bind_text(stmt, 1, session_token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<UserRecord> res;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        UserRecord u;
        u.id = safe_col_text(stmt, 0);
        u.email = safe_col_text(stmt, 1);
        u.name = safe_col_text(stmt, 2);
        u.role = safe_col_text(stmt, 3);
        u.is_active = (sqlite3_column_int(stmt, 4) != 0);
        u.created_at = safe_col_text(stmt, 5);
        u.last_login_at = safe_col_text(stmt, 6);
        res = u;
    }
    sqlite3_finalize(stmt);
    return res;
}

bool AuthSmtpService::revoke_session(sqlite3* db, const std::string& session_token) {
    if (!db || session_token.empty()) return false;
    const char* sql = "DELETE FROM sessions WHERE token=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, session_token.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

} // namespace evida::auth
