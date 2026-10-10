#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <thread>

#include "fomapay/fomapay.hpp"
#include "internal.hpp"

namespace fomapay {

namespace {

constexpr long MAX_BACKOFF_MS = 8000;

std::mt19937& rng() {
    thread_local std::mt19937 generator{std::random_device{}()};
    return generator;
}

/// Clé unique : rejouer la requête ne crée pas de doublon.
std::string new_idempotency_key() {
    static const char* hex = "0123456789abcdef";
    std::uniform_int_distribution<int> digit(0, 15);
    std::string key = "cpp-";
    for (int i = 0; i < 32; ++i) {
        key += hex[digit(rng())];
    }
    return key;
}

/// 0,5 s, 1 s, 2 s… plafonné à 8 s, avec un aléa pour désynchroniser les clients.
long backoff_ms(int attempt) {
    const long delay = std::min(500L << std::min(attempt, 10), MAX_BACKOFF_MS);
    return delay / 2 + std::uniform_int_distribution<long>(0, delay / 2)(rng());
}

/// Encode un segment de chemin ou une valeur de requête (RFC 3986).
std::string encode(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof buffer, "%%%02X", static_cast<unsigned>(c));
            out += buffer;
        }
    }
    return out;
}

std::vector<std::string> strings(const json::Value& object, const std::string& key) {
    std::vector<std::string> out;
    if (const json::Value* list = object.find(key); list && list->is_array()) {
        for (const json::Value& item : list->array) {
            if (item.type == json::Value::Type::String) {
                out.push_back(item.string);
            }
        }
    }
    return out;
}

json::Value parse_object(const std::string& body, int status) {
    try {
        json::Value root = json::parse(body);
        if (root.is_object()) {
            return root;
        }
    } catch (const std::invalid_argument&) {
        // traité ci-dessous
    }
    throw ApiError("ERROR_INVALID_RESPONSE", "Réponse illisible de l'API FomaPay.", status);
}

ApiError api_error(const HttpResponse& response) {
    std::string code = "ERROR_UNKNOWN";
    std::string message = "Erreur HTTP " + std::to_string(response.status);
    try {
        const json::Value root = json::parse(response.body);
        if (const json::Value* error = root.find("error"); error && error->is_object()) {
            if (!error->str("code").empty()) {
                code = error->str("code");
            }
            if (!error->str("message").empty()) {
                message = error->str("message");
            }
        }
    } catch (const std::invalid_argument&) {
        // corps non JSON (proxy, page d'erreur…) : on garde le statut HTTP
    }
    return ApiError(code, message, response.status);
}

template <typename T, typename Convert>
std::vector<T> list_of(const std::string& body, Convert convert) {
    std::vector<T> out;
    const json::Value root = parse_object(body, 200);
    if (const json::Value* data = root.find("data"); data && data->is_array()) {
        for (const json::Value& item : data->array) {
            if (item.is_object()) {
                out.push_back(convert(item));
            }
        }
    }
    return out;
}

}  // namespace

Payment internal::to_payment(const json::Value& v) {
    Payment p;
    p.id = v.str("id");
    p.reference = v.str("reference");
    p.status = v.str("status");
    p.amount = v.num("amount");
    p.fee = v.num("fee");
    p.net = v.num("net");
    p.currency = v.str("currency");
    p.channel = v.str("channel");
    p.order_id = v.str("orderId");
    p.description = v.str("description");
    if (const json::Value* customer = v.find("customer"); customer && customer->is_object()) {
        p.customer.name = customer->str("name");
        p.customer.phone = customer->str("phone");
        p.customer.email = customer->str("email");
    }
    if (const json::Value* metadata = v.find("metadata"); metadata && metadata->is_object()) {
        for (const auto& member : metadata->object) {
            if (member.second.type == json::Value::Type::String) {
                p.metadata[member.first] = member.second.string;
            }
        }
    }
    p.payment_modes = strings(v, "paymentModes");
    p.callback_url = v.str("callbackUrl");
    p.payer_phone = v.str("payerPhone");
    p.qr_code = v.str("qrCode");
    p.livemode = v.flag("livemode");
    p.store_id = v.str("storeId");
    p.failure_code = v.str("failureCode");
    p.failure_reason = v.str("failureReason");
    p.expires_at = v.str("expiresAt");
    p.completed_at = v.str("completedAt");
    p.created_at = v.str("createdAt");
    return p;
}

Client::Client(std::string secret_key, ClientOptions options) : secret_key_(std::move(secret_key)), options_(std::move(options)) {
    if (secret_key_.compare(0, 8, "sk_test_") != 0 && secret_key_.compare(0, 8, "sk_live_") != 0) {
        throw Error("ERROR_INVALID_API_KEY",
                    "Clé secrète absente ou mal formée : elle commence par sk_test_ ou sk_live_ (FomaPay Business › Développeurs).");
    }
    while (!options_.base_url.empty() && options_.base_url.back() == '/') {
        options_.base_url.pop_back();
    }
    options_.max_retries = std::max(0, options_.max_retries);
    if (!options_.transport) {
        options_.transport = internal::make_curl_transport();
    }
    if (!options_.sleep) {
        options_.sleep = [](long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    }
}

bool Client::livemode() const { return secret_key_.compare(0, 8, "sk_live_") == 0; }

/// Exécute une requête, avec nouvelles tentatives quand elle est rejouable sans risque
/// (lecture, ou POST accompagné d'une clé d'idempotence).
std::string Client::request(const std::string& method, const std::string& path, const std::string& body,
                            const std::string& idempotency_key) const {
    HttpRequest request;
    request.method = method;
    request.url = options_.base_url + path;
    request.body = body;
    request.timeout_ms = options_.timeout_ms;
    request.headers = {{"Authorization", "Bearer " + secret_key_},
                       {"Accept", "application/json"},
                       {"User-Agent", std::string("libfomapay/") + VERSION}};
    if (!body.empty()) {
        request.headers.emplace_back("Content-Type", "application/json");
    }
    if (!idempotency_key.empty()) {
        request.headers.emplace_back("Idempotency-Key", idempotency_key);
    }
    const bool replayable = method == "GET" || !idempotency_key.empty();

    for (int attempt = 0;; ++attempt) {
        long wait_ms = -1;
        try {
            const HttpResponse response = options_.transport->send(request);
            if (response.status >= 200 && response.status < 300) {
                return response.body;
            }
            bool retry = response.status >= 500 && replayable;
            if (response.status == 429) {
                // La gateway a refusé la requête avant tout traitement : toujours rejouable.
                retry = true;
                if (response.retry_after > 0) {
                    wait_ms = std::min(response.retry_after * 1000, MAX_BACKOFF_MS);
                }
            }
            if (!retry || attempt >= options_.max_retries) {
                throw api_error(response);
            }
        } catch (const NetworkError&) {
            if (!replayable || attempt >= options_.max_retries) {
                throw;
            }
        }
        options_.sleep(wait_ms >= 0 ? wait_ms : backoff_ms(attempt));
    }
}

Payment Client::create_payment(const PaymentParams& params) const {
    json::Writer body;
    body.number("amount", params.amount).string("currency", params.currency);
    body.optional("orderId", params.order_id).optional("description", params.description);

    json::Writer customer;
    customer.optional("name", params.customer.name).optional("phone", params.customer.phone).optional("email", params.customer.email);
    if (!customer.empty()) {
        body.raw("customer", customer.str());
    }
    if (!params.payment_modes.empty()) {
        std::string modes;
        for (const std::string& mode : params.payment_modes) {
            modes += (modes.empty() ? "" : ",") + json::quote(mode);
        }
        body.raw("paymentModes", "[" + modes + "]");
    }
    body.optional("callbackUrl", params.callback_url);
    if (!params.metadata.empty()) {
        json::Writer metadata;
        for (const auto& entry : params.metadata) {
            metadata.string(entry.first, entry.second);
        }
        body.raw("metadata", metadata.str());
    }

    const std::string key = params.idempotency_key.empty() ? new_idempotency_key() : params.idempotency_key;
    return internal::to_payment(parse_object(request("POST", "/v1/payments", body.str(), key), 200));
}

Payment Client::retrieve_payment(const std::string& id) const {
    return internal::to_payment(parse_object(request("GET", "/v1/payments/" + encode(id), "", ""), 200));
}

std::vector<Payment> Client::list_payments(int limit, const std::string& status) const {
    std::string query;
    if (limit > 0) {
        query += "limit=" + std::to_string(limit);
    }
    if (!status.empty()) {
        query += (query.empty() ? "" : "&") + ("status=" + encode(status));
    }
    return list_of<Payment>(request("GET", "/v1/payments" + (query.empty() ? "" : "?" + query), "", ""), internal::to_payment);
}

Payment Client::pay_with_mobile_money(const std::string& id, const std::string& op, const std::string& phone) const {
    const std::string body = json::Writer().string("operator", op).string("phone", phone).str();
    return internal::to_payment(parse_object(request("POST", "/v1/payments/" + encode(id) + "/mobile-money", body, ""), 200));
}

Payment Client::simulate_payment(const std::string& id, const std::string& outcome) const {
    const std::string body = json::Writer().string("outcome", outcome).str();
    return internal::to_payment(parse_object(request("POST", "/v1/payments/" + encode(id) + "/simulate", body, ""), 200));
}

Payment Client::create_payout(const PayoutParams& params) const {
    json::Writer body;
    body.number("amount", params.amount).string("currency", params.currency).string("operator", params.op).string("phone", params.phone);
    body.optional("description", params.description);
    // Pas de clé d'idempotence : un retrait n'est jamais rejoué automatiquement.
    return internal::to_payment(parse_object(request("POST", "/v1/payouts", body.str(), ""), 200));
}

std::vector<Balance> Client::balance() const {
    return list_of<Balance>(request("GET", "/v1/balance", "", ""), [](const json::Value& v) {
        Balance b;
        b.currency = v.str("currency");
        b.available = v.num("available");
        return b;
    });
}

Store Client::store() const {
    const json::Value v = parse_object(request("GET", "/v1/store", "", ""), 200);
    Store s;
    s.id = v.str("id");
    s.name = v.str("name");
    s.short_code = v.str("shortCode");
    s.logo_url = v.str("logoUrl");
    s.livemode = v.flag("livemode");
    s.kyc_level = static_cast<int>(v.num("kycLevel"));
    s.payment_modes = strings(v, "paymentModes");
    return s;
}

}  // namespace fomapay
