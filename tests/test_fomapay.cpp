// Tests de libfomapay, sans framework : chaque CHECK compte, le programme renvoie 1 au moindre échec.
//
//   ctest --output-on-failure
//
// Le test d'intégration ne s'exécute que si FOMAPAY_TEST_SECRET_KEY (sk_test_…) est définie ;
// FOMAPAY_API_URL cible un backend local.
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <thread>

#include "fomapay/fomapay.hpp"
#include "json.hpp"

namespace {

int checks = 0;
int failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        ++checks;                                                                           \
        if (!(condition)) {                                                                 \
            ++failures;                                                                     \
            std::cerr << "  ECHEC " << __FILE__ << ":" << __LINE__ << "  " #condition "\n"; \
        }                                                                                   \
    } while (0)

/// Vérifie qu'une expression lève `Type` avec le code d'erreur attendu.
#define CHECK_THROWS(expression, Type, expected_code)                 \
    do {                                                              \
        bool thrown = false;                                          \
        try {                                                         \
            (void)(expression);                                       \
        } catch (const Type& e) {                                     \
            thrown = std::string(e.code()) == (expected_code);        \
            if (!thrown) std::cerr << "  code obtenu : " << e.code() << "\n"; \
        } catch (const std::exception& e) {                           \
            std::cerr << "  autre exception : " << e.what() << "\n";  \
        }                                                             \
        CHECK(thrown);                                                \
    } while (0)

const char* PAYMENT = R"({"id":"pay_1","reference":"FP-1","status":"pending","amount":25,"currency":"USD","orderId":"CMD-1","champInconnu":1})";

fomapay::HttpResponse ok(const std::string& body, int status = 200) {
    fomapay::HttpResponse response;
    response.status = status;
    response.body = body;
    return response;
}

fomapay::HttpResponse failure(int status, const std::string& code, long retry_after = 0) {
    fomapay::HttpResponse response = ok(R"({"error":{"code":")" + code + R"(","message":"oups"}})", status);
    response.retry_after = retry_after;
    return response;
}

/// Transport factice : mémorise les requêtes et délègue la réponse au test.
struct FakeTransport : fomapay::HttpTransport {
    std::function<fomapay::HttpResponse(int, const fomapay::HttpRequest&)> respond;
    std::vector<fomapay::HttpRequest> calls;

    fomapay::HttpResponse send(const fomapay::HttpRequest& request) override {
        calls.push_back(request);
        return respond(static_cast<int>(calls.size()), request);
    }

    std::string header(std::size_t call, const std::string& name) const {
        for (const auto& h : calls.at(call).headers) {
            if (h.first == name) {
                return h.second;
            }
        }
        return "";
    }
};

struct Fixture {
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::vector<long> waits;

    fomapay::Client client(std::function<fomapay::HttpResponse(int, const fomapay::HttpRequest&)> respond, int max_retries = 2) {
        transport->respond = std::move(respond);
        fomapay::ClientOptions options;
        options.base_url = "https://api.test/";
        options.max_retries = max_retries;
        options.transport = transport;
        options.sleep = [this](long ms) { waits.push_back(ms); };
        return fomapay::Client("sk_test_unit", options);
    }
};

fomapay::PayoutParams payout() {
    fomapay::PayoutParams params;
    params.amount = 10;
    params.op = fomapay::MPESA;
    params.phone = "+243810000000";
    return params;
}

void test_json() {
    const auto root = fomapay::json::parse(R"( {"a":[1,-2.5e1,true,null,"é\n\"x\"é😀"],"b":{}} )");
    CHECK(root.is_object() && root.find("a")->array.size() == 5);
    CHECK(root.find("a")->array[1].number == -25);
    CHECK(root.find("a")->array[4].string == "é\n\"x\"é\xF0\x9F\x98\x80");
    CHECK(fomapay::json::Writer().number("n", 12.5).string("s", "a\"b").str() == R"({"n":12.5,"s":"a\"b"})");
    for (const char* invalid : {"{\"a\":", "<html>", "{\"a\":1}x", "[1,]", ""}) {
        bool thrown = false;
        try {
            fomapay::json::parse(invalid);
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        CHECK(thrown);
    }
}

void test_create_sends_standard_format() {
    Fixture f;
    const fomapay::Client client = f.client([](int, const fomapay::HttpRequest&) { return ok(PAYMENT, 201); });

    fomapay::PaymentParams params;
    params.amount = 25;
    params.currency = fomapay::USD;
    params.order_id = "CMD-1";
    params.customer.name = "Jean Marc";
    params.customer.phone = "+243810000000";
    params.payment_modes = {fomapay::FOMAPAY_DIRECT, fomapay::MOBILE_MONEY};
    params.callback_url = "https://marchand.cd/api/callback";
    params.metadata["custom_field"] = "valeur";
    const fomapay::Payment payment = client.create_transaction(params);

    const fomapay::HttpRequest& call = f.transport->calls.at(0);
    CHECK(call.method == "POST" && call.url == "https://api.test/v1/payments");
    CHECK(f.transport->header(0, "Authorization") == "Bearer sk_test_unit");
    CHECK(f.transport->header(0, "Idempotency-Key").compare(0, 4, "cpp-") == 0 && f.transport->header(0, "Idempotency-Key").size() == 36);
    CHECK(call.body ==
          R"({"amount":25,"currency":"USD","orderId":"CMD-1","customer":{"name":"Jean Marc","phone":"+243810000000"},)"
          R"("paymentModes":["fomapay_direct","mobile_money"],"callbackUrl":"https://marchand.cd/api/callback",)"
          R"("metadata":{"custom_field":"valeur"}})");
    CHECK(call.timeout_ms == 30000);
    CHECK(payment.id == "pay_1" && payment.order_id == "CMD-1" && payment.amount == 25);
    CHECK(payment.pending() && !payment.succeeded());
    CHECK_THROWS(fomapay::Client("pk_test_abc"), fomapay::Error, "ERROR_INVALID_API_KEY");
}

void test_retries() {
    {
        Fixture f;  // erreurs serveur puis coupure : rejouées avec la même clé d'idempotence
        const fomapay::Client client = f.client([](int n, const fomapay::HttpRequest&) -> fomapay::HttpResponse {
            if (n == 1) return failure(503, "ERROR_UNAVAILABLE");
            if (n == 2) throw fomapay::NetworkError("ERROR_NETWORK", "coupure");
            return ok(PAYMENT, 201);
        });
        fomapay::PaymentParams params;
        params.amount = 25;
        CHECK(client.create_payment(params).id == "pay_1");
        CHECK(f.transport->calls.size() == 3);
        CHECK(f.transport->header(0, "Idempotency-Key") == f.transport->header(2, "Idempotency-Key"));
        CHECK(f.waits.size() == 2 && f.waits[0] >= 250 && f.waits[0] <= 500 && f.waits[1] >= 500 && f.waits[1] <= 1000);
    }
    {
        Fixture f;  // abandon après max_retries
        const fomapay::Client client = f.client([](int, const fomapay::HttpRequest&) { return failure(500, "ERROR_INTERNAL"); }, 1);
        CHECK_THROWS(client.retrieve_payment("FP-1"), fomapay::ApiError, "ERROR_INTERNAL");
        CHECK(f.transport->calls.size() == 2);
    }
    {
        Fixture f;  // les erreurs 4xx ne sont pas rejouées
        const fomapay::Client client = f.client([](int, const fomapay::HttpRequest&) { return failure(403, "ERROR_LEVEL_0_LIMIT_REACHED"); });
        fomapay::PaymentParams params;
        params.amount = 900;
        try {
            client.create_payment(params);
            CHECK(false);
        } catch (const fomapay::ApiError& e) {
            CHECK(e.code() == "ERROR_LEVEL_0_LIMIT_REACHED" && e.status() == 403);
        }
        CHECK(f.transport->calls.size() == 1);
    }
    {
        Fixture f;  // un retrait n'est jamais rejoué…
        const fomapay::Client client = f.client([](int, const fomapay::HttpRequest&) { return failure(502, "ERROR_BAD_GATEWAY"); });
        CHECK_THROWS(client.create_payout(payout()), fomapay::ApiError, "ERROR_BAD_GATEWAY");
        CHECK(f.transport->calls.size() == 1 && f.transport->header(0, "Idempotency-Key").empty());
        CHECK(f.transport->calls[0].body == R"({"amount":10,"currency":"USD","operator":"mpesa","phone":"+243810000000"})");
    }
    {
        Fixture f;
        const fomapay::Client client = f.client([](int, const fomapay::HttpRequest&) -> fomapay::HttpResponse {
            throw fomapay::NetworkError("ERROR_TIMEOUT", "trop long");
        });
        CHECK_THROWS(client.create_payout(payout()), fomapay::NetworkError, "ERROR_TIMEOUT");
        CHECK(f.transport->calls.size() == 1);
        CHECK_THROWS(client.balance(), fomapay::NetworkError, "ERROR_TIMEOUT");  // une lecture, elle, est rejouée
        CHECK(f.transport->calls.size() == 4);
    }
    {
        Fixture f;  // …sauf sur un 429, en respectant Retry-After
        const fomapay::Client client = f.client([](int n, const fomapay::HttpRequest&) {
            return n == 1 ? failure(429, "ERROR_RATE_LIMITED", 2) : ok(PAYMENT, 201);
        });
        client.create_payout(payout());
        CHECK(f.transport->calls.size() == 2 && f.waits == std::vector<long>{2000});
    }
    {
        Fixture f;  // réponse illisible (page HTML d'un proxy)
        const fomapay::Client client = f.client([](int, const fomapay::HttpRequest&) { return ok("<html>", 502); }, 0);
        CHECK_THROWS(client.store(), fomapay::ApiError, "ERROR_UNKNOWN");
    }
}

void test_other_resources() {
    Fixture f;
    const fomapay::Client client = f.client([](int, const fomapay::HttpRequest& request) {
        if (request.url.find("/v1/payments?") != std::string::npos) return ok(std::string(R"({"data":[)") + PAYMENT + "]}");
        if (request.url.find("/v1/balance") != std::string::npos) return ok(R"({"data":[{"currency":"USD","available":12.5}]})");
        if (request.url.find("/v1/store") != std::string::npos) {
            return ok(R"({"id":"s1","name":"Chez Foma","shortCode":"FOMA","kycLevel":2,"paymentModes":["fomapay_direct"]})");
        }
        return ok(R"({"id":"pay_1","status":"success","customer":{"name":"Aline"},"metadata":{"k":"v"},"livemode":true})");
    });

    CHECK(client.list_payments(5, "success").size() == 1);
    CHECK(client.balance().at(0).available == 12.5);
    const fomapay::Store store = client.store();
    CHECK(store.short_code == "FOMA" && store.kyc_level == 2 && store.payment_modes.size() == 1);
    client.pay_with_mobile_money("FP/1", fomapay::AIRTEL_MONEY, "+243970000000");
    const fomapay::Payment paid = client.simulate_payment("pay_1");
    CHECK(paid.succeeded() && paid.customer.name == "Aline" && paid.metadata.at("k") == "v" && paid.livemode);

    CHECK(f.transport->calls[0].url == "https://api.test/v1/payments?limit=5&status=success");
    CHECK(f.transport->calls[3].url == "https://api.test/v1/payments/FP%2F1/mobile-money");
    CHECK(f.transport->calls[3].body == R"({"operator":"airtel_money","phone":"+243970000000"})");
    CHECK(f.transport->calls[4].body == R"({"outcome":"success"})");
    CHECK(!client.livemode());
}

void test_webhooks() {
    namespace webhook = fomapay::webhook;
    const std::string secret = "whsec_unit";
    const long long now = 1800000000;
    const std::string body =
        R"({"id":"evt_1","type":"payment.succeeded","created":"2026-10-10T10:00:00Z","livemode":false,"data":{"id":"pay_1","status":"success","orderId":"CMD-1"}})";
    const auto sign = [&](const std::string& payload, const std::string& key) {
        return "t=" + std::to_string(now) + ",v1=" + webhook::hmac_sha256_hex(key, std::to_string(now) + "." + payload);
    };

    // Vecteur commun à tous les SDKs FomaPay, calculé avec OpenSSL :
    // printf '1700000000.{"id":"evt_1"}' | openssl dgst -sha256 -hmac whsec_test
    const std::string digest = "c89214b5b5da833daed6f0b8c5bb6bd58cea9022bd80ccc78230f3942d632925";
    CHECK(webhook::hmac_sha256_hex("whsec_test", R"(1700000000.{"id":"evt_1"})") == digest);
    CHECK(webhook::verify(R"({"id":"evt_1"})", "t=1700000000,v1=" + digest, "whsec_test", 300, 1700000000));
    // RFC 4231, cas 2 et cas 6 (clé plus longue qu'un bloc).
    CHECK(webhook::hmac_sha256_hex("Jefe", "what do ya want for nothing?") == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(webhook::hmac_sha256_hex(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First") ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    const fomapay::Event event = webhook::construct_event(body, sign(body, secret), secret, 300, now);
    CHECK(event.type == "payment.succeeded" && event.data.succeeded() && event.data.order_id == "CMD-1");
    CHECK(webhook::verify(body, sign(body, secret), secret, 300, now + 299));

    std::string upper = sign(body, secret);
    for (std::size_t i = upper.find("v1=") + 3; i < upper.size(); ++i) {
        upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(upper[i])));
    }
    CHECK(webhook::verify(body, upper, secret, 300, now));

    CHECK(!webhook::verify(body + " ", sign(body, secret), secret, 300, now));        // corps modifié
    CHECK(!webhook::verify(body, sign(body, "whsec_autre"), secret, 300, now));       // mauvais secret
    CHECK(!webhook::verify(body, sign(body, secret), secret, 300, now + 301));        // trop ancien
    CHECK(!webhook::verify(body, sign(body, secret), secret, 300, now - 301));        // horodatage dans le futur
    for (const char* header : {"", "t=abc,v1=00", "t=1800000000", "v1=00"}) {
        CHECK(!webhook::verify(body, header, secret, 300, now));
    }
    CHECK_THROWS(webhook::construct_event(body, "t=1,v1=00", secret), fomapay::SignatureError, "ERROR_INVALID_SIGNATURE");
    CHECK_THROWS(webhook::construct_event("pas du json", sign("pas du json", secret), secret, 300, now), fomapay::SignatureError,
                 "ERROR_INVALID_PAYLOAD");
    CHECK_THROWS(webhook::verify(body, sign(body, secret), ""), fomapay::Error, "ERROR_WEBHOOK_SECRET_MISSING");
}

/// Contre le sandbox, avec le vrai transport libcurl.
void test_integration(const std::string& key) {
    fomapay::ClientOptions options;
    if (const char* url = std::getenv("FOMAPAY_API_URL"); url && *url) {
        options.base_url = url;
    }
    const fomapay::Client client(key, options);

    const fomapay::Store store = client.store();
    CHECK(!store.livemode && store.payment_modes.size() == 2);

    // Format standard d'une transaction, dans les deux modes.
    fomapay::PaymentParams params;
    params.amount = 1000;
    params.currency = fomapay::CDF;
    params.order_id = "CMD-2026-001";
    params.customer = {"Jean Marc", "+243810000000", "jean@example.com"};
    params.payment_modes = {fomapay::FOMAPAY_DIRECT, fomapay::MOBILE_MONEY};
    params.callback_url = "https://marchand.cd/api/callback";
    params.metadata["custom_field"] = "valeur";
    const fomapay::Payment payment = client.create_payment(params);
    CHECK(payment.status == "pending" && payment.callback_url == "https://marchand.cd/api/callback" && !payment.qr_code.empty());
    CHECK(payment.metadata.count("custom_field") == 1 && payment.customer.name == "Jean Marc");

    // Mode Mobile Money : l'opérateur simulé confirme après ~3 s.
    fomapay::Payment last = client.pay_with_mobile_money(payment.id, fomapay::AIRTEL_MONEY, "+243970000000");
    CHECK(last.status == "processing");
    for (int i = 0; i < 10 && last.pending(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        last = client.retrieve_payment(payment.reference);
    }
    CHECK(last.succeeded() && last.channel == "airtel_money");

    // Mode FomaPay Direct : issue forcée en sandbox.
    fomapay::PaymentParams direct;
    direct.amount = 12.5;
    direct.payment_modes = {fomapay::FOMAPAY_DIRECT};
    CHECK(client.simulate_payment(client.create_payment(direct).id, "success").succeeded());

    // Idempotence : la même clé renvoie le même paiement.
    fomapay::PaymentParams same;
    same.amount = 3;
    same.idempotency_key = "cpp-it-" + payment.id;
    CHECK(client.create_payment(same).id == client.create_payment(same).id);

    CHECK(!client.list_payments(5, "success").empty());
    CHECK(!client.balance().empty());

    fomapay::PaymentParams negative;
    negative.amount = -1;
    CHECK_THROWS(client.create_payment(negative), fomapay::ApiError, "ERROR_VALIDATION");
    CHECK_THROWS(client.retrieve_payment("FP-INEXISTANT"), fomapay::ApiError, "ERROR_NOT_FOUND");

    // Erreur réseau réelle : port fermé, lecture rejouée puis abandonnée.
    fomapay::ClientOptions offline;
    offline.base_url = "http://127.0.0.1:1";
    offline.max_retries = 1;
    offline.sleep = [](long) {};
    CHECK_THROWS(fomapay::Client(key, offline).store(), fomapay::NetworkError, "ERROR_NETWORK");
}

void run(const char* name, const std::function<void()>& test) {
    const int before = failures;
    try {
        test();
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << "  exception inattendue : " << e.what() << "\n";
    }
    std::cout << (failures == before ? "ok     " : "ECHEC  ") << name << "\n";
}

}  // namespace

int main() {
    run("json", test_json);
    run("create : format standard et idempotence", test_create_sends_standard_format);
    run("nouvelles tentatives", test_retries);
    run("autres ressources", test_other_resources);
    run("webhooks", test_webhooks);

    const char* key = std::getenv("FOMAPAY_TEST_SECRET_KEY");
    if (key && *key) {
        run("integration sandbox (libcurl)", [key] { test_integration(key); });
    } else {
        std::cout << "ignore integration sandbox (FOMAPAY_TEST_SECRET_KEY non definie)\n";
    }

    std::cout << checks << " verifications, " << failures << " echec(s)\n";
    return failures == 0 ? 0 : 1;
}
