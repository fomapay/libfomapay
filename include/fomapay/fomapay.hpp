// libfomapay — SDK C++ officiel de FomaPay (FomaPay Direct et Mobile Money, RDC).
//
//   fomapay::Client client("sk_test_…");
//   fomapay::PaymentParams params;
//   params.amount = 1000;
//   params.currency = fomapay::CDF;
//   fomapay::Payment payment = client.create_payment(params);
//
// C++17, une seule dépendance : libcurl.
#ifndef FOMAPAY_FOMAPAY_HPP
#define FOMAPAY_FOMAPAY_HPP

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fomapay {

inline constexpr const char* VERSION = "0.1.0";
inline constexpr const char* DEFAULT_BASE_URL = "https://api.fomapay.cd";

// Devises.
inline constexpr const char* USD = "USD";
inline constexpr const char* CDF = "CDF";

// Modes de paiement. FomaPay Direct est disponible dès le Niveau 0 ;
// Mobile Money est réservé aux boutiques vérifiées (Niveau 1+).
inline constexpr const char* FOMAPAY_DIRECT = "fomapay_direct";
inline constexpr const char* MOBILE_MONEY = "mobile_money";

// Opérateurs Mobile Money.
inline constexpr const char* MPESA = "mpesa";
inline constexpr const char* ORANGE_MONEY = "orange_money";
inline constexpr const char* AIRTEL_MONEY = "airtel_money";

// ───────────────────────────── Erreurs ─────────────────────────────

/// Erreur de base. code() est stable et exploitable par le programme
/// (ex. ERROR_LEVEL_0_LIMIT_REACHED) ; what() est lisible par un humain.
class Error : public std::runtime_error {
public:
    Error(std::string code, const std::string& message, int status = 0)
        : std::runtime_error(code + ": " + message), code_(std::move(code)), status_(status) {}

    const std::string& code() const noexcept { return code_; }
    /// Statut HTTP (0 quand il n'y a pas eu de réponse).
    int status() const noexcept { return status_; }

private:
    std::string code_;
    int status_;
};

/// L'API a répondu avec une erreur.
class ApiError : public Error {
public:
    using Error::Error;
};

/// Aucune réponse : connexion impossible (ERROR_NETWORK) ou délai dépassé (ERROR_TIMEOUT).
class NetworkError : public Error {
public:
    using Error::Error;
};

/// Signature de webhook invalide ou expirée.
class SignatureError : public Error {
public:
    using Error::Error;
};

// ───────────────────────────── Modèles ─────────────────────────────

struct Customer {
    std::string name;
    std::string phone;
    std::string email;
};

/// Paramètres de création d'un paiement. Les champs laissés vides ne sont pas envoyés.
struct PaymentParams {
    /// Montant en unités de la devise (12.5 = 12,50 USD).
    double amount = 0;
    std::string currency = USD;
    /// Votre identifiant de commande, renvoyé tel quel dans les webhooks.
    std::string order_id;
    std::string description;
    Customer customer;
    std::vector<std::string> payment_modes;
    /// Page de votre site vers laquelle renvoyer le client une fois le paiement terminé.
    std::string callback_url;
    std::map<std::string, std::string> metadata;
    /// Ex. l'identifiant de commande : un appel répété renvoie le même paiement.
    std::string idempotency_key;
};

/// Paramètres d'un retrait du solde de la boutique vers un compte Mobile Money.
struct PayoutParams {
    double amount = 0;
    std::string currency = USD;
    std::string op;  ///< Opérateur : MPESA, ORANGE_MONEY ou AIRTEL_MONEY.
    std::string phone;
    std::string description;
};

/// Un paiement, un retrait ou tout autre mouvement. Les champs absents restent vides.
struct Payment {
    std::string id;
    std::string reference;
    std::string status;  ///< pending, processing, success, failed ou expired.
    double amount = 0;
    double fee = 0;
    double net = 0;
    std::string currency;
    std::string channel;
    std::string order_id;
    std::string description;
    Customer customer;
    std::map<std::string, std::string> metadata;
    std::vector<std::string> payment_modes;
    std::string callback_url;
    std::string payer_phone;
    /// Contenu du QR code à afficher au client (FomaPay Direct).
    std::string qr_code;
    bool livemode = false;
    std::string store_id;
    std::string failure_code;
    std::string failure_reason;
    std::string expires_at;
    std::string completed_at;
    std::string created_at;

    bool succeeded() const { return status == "success"; }
    /// Le paiement attend encore le client ou l'opérateur.
    bool pending() const { return status == "pending" || status == "processing"; }
};

struct Balance {
    std::string currency;
    double available = 0;
};

/// Boutique liée à la clé, avec les modes que son niveau KYC autorise.
struct Store {
    std::string id;
    std::string name;
    std::string short_code;
    std::string logo_url;
    bool livemode = false;
    int kyc_level = 0;
    std::vector<std::string> payment_modes;
};

/// Événement de webhook : payment.succeeded, payment.failed, payout.succeeded…
struct Event {
    std::string id;
    std::string type;
    std::string created;
    bool livemode = false;
    Payment data;
};

// ───────────────────────────── Transport ─────────────────────────────

struct HttpRequest {
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    long timeout_ms = 30000;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    /// Valeur de l'en-tête Retry-After, en secondes (0 si absent).
    long retry_after = 0;
};

/// Transport HTTP. L'implémentation par défaut utilise libcurl ; fournissez la vôtre
/// pour un environnement embarqué ou pour les tests. Doit lever NetworkError sans réponse.
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse send(const HttpRequest& request) = 0;
};

// ───────────────────────────── Client ─────────────────────────────

struct ClientOptions {
    /// À surcharger pour pointer vers un backend local en développement.
    std::string base_url = DEFAULT_BASE_URL;
    /// Délai maximal d'une requête, en millisecondes.
    long timeout_ms = 30000;
    /// Nouvelles tentatives après un échec rejouable (0 pour désactiver).
    int max_retries = 2;
    /// Transport à utiliser (libcurl par défaut).
    std::shared_ptr<HttpTransport> transport;
    /// Attente entre deux tentatives, en millisecondes ; remplaçable dans les tests.
    std::function<void(long)> sleep;
};

/// Client serveur FomaPay. Une instance peut être partagée entre threads.
class Client {
public:
    /// @param secret_key clé secrète de la boutique (sk_test_… ou sk_live_…)
    /// @throws Error ERROR_INVALID_API_KEY si la clé est mal formée
    explicit Client(std::string secret_key, ClientOptions options = {});

    /// Crée une demande de paiement. Porte toujours une clé d'idempotence : rejouable sans doublon.
    Payment create_payment(const PaymentParams& params) const;
    /// Alias de create_payment.
    Payment create_transaction(const PaymentParams& params) const { return create_payment(params); }
    /// Relit un paiement par son identifiant ou sa référence (FP-…).
    Payment retrieve_payment(const std::string& id) const;
    /// Les derniers paiements de la boutique (limit = 0 : valeur par défaut de l'API).
    std::vector<Payment> list_payments(int limit = 0, const std::string& status = "") const;
    /// Envoie la demande de débit (push USSD) sur le téléphone du client.
    Payment pay_with_mobile_money(const std::string& id, const std::string& op, const std::string& phone) const;
    /// Force l'issue d'un paiement (success ou failed). Sandbox uniquement.
    Payment simulate_payment(const std::string& id, const std::string& outcome = "success") const;
    /// Retrait vers Mobile Money. Jamais rejoué automatiquement : un retrait ne doit pas partir deux fois.
    Payment create_payout(const PayoutParams& params) const;
    /// Un solde par devise.
    std::vector<Balance> balance() const;
    Store store() const;

    bool livemode() const;

private:
    std::string request(const std::string& method, const std::string& path, const std::string& body,
                        const std::string& idempotency_key) const;

    std::string secret_key_;
    ClientOptions options_;
};

// ───────────────────────────── Webhooks ─────────────────────────────

namespace webhook {

/// En-tête HTTP qui porte la signature.
inline constexpr const char* SIGNATURE_HEADER = "X-FomaPay-Signature";
/// Âge maximal accepté pour un événement, en secondes (anti-rejeu).
inline constexpr long DEFAULT_TOLERANCE = 300;

/// Indique si la signature est valide pour ce corps BRUT.
/// @param header valeur de l'en-tête X-FomaPay-Signature : « t=<horodatage>,v1=<hmac> »
/// @param now    horodatage courant ; 0 = heure du système (paramètre réservé aux tests)
bool verify(const std::string& payload, const std::string& header, const std::string& secret,
            long tolerance = DEFAULT_TOLERANCE, long long now = 0);

/// Vérifie la signature puis renvoie l'événement.
/// @throws SignatureError si la signature est invalide ou expirée
Event construct_event(const std::string& payload, const std::string& header, const std::string& secret,
                      long tolerance = DEFAULT_TOLERANCE, long long now = 0);

/// HMAC-SHA256 en hexadécimal minuscule (exposé pour signer vos propres jeux d'essai).
std::string hmac_sha256_hex(const std::string& key, const std::string& message);

}  // namespace webhook

}  // namespace fomapay

#endif  // FOMAPAY_FOMAPAY_HPP
