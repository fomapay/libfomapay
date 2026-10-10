# libfomapay

SDK C++ officiel de **FomaPay** : encaissez en RDC par **FomaPay Direct** (QR / code marchand, dès le Niveau 0) et par **Mobile Money** (M-Pesa, Orange Money, Airtel Money, dès le Niveau 1).

Conçu pour les terminaux de paiement, les bornes, l'embarqué et les applications Qt :

- C++17, **une seule dépendance : libcurl** (JSON et HMAC-SHA256 intégrés, sans OpenSSL ni bibliothèque JSON)
- Bibliothèque statique d'environ 270 Ko, aucune allocation globale, aucun thread caché
- Nouvelles tentatives automatiques et délai d'attente
- Vérification des webhooks (HMAC-SHA256)
- Transport remplaçable (`HttpTransport`) pour les plateformes sans libcurl

> Le SDK utilise votre clé secrète `sk_…`. Sur un terminal, lisez-la depuis un stockage protégé ; ne la compilez jamais dans le binaire.

## Intégration en 3 étapes

### 1. Ajouter la bibliothèque

```cmake
include(FetchContent)
FetchContent_Declare(fomapay GIT_REPOSITORY https://github.com/fomapay/libfomapay.git GIT_TAG v0.1.0)
set(FOMAPAY_BUILD_TESTS OFF)
set(FOMAPAY_BUILD_EXAMPLES OFF)
FetchContent_MakeAvailable(fomapay)

target_link_libraries(mon_application PRIVATE fomapay::fomapay)
```

Prérequis : les en-têtes de libcurl (`apt install libcurl4-openssl-dev`, `apk add curl-dev`, `vcpkg install curl`).

### 2. Créer un paiement

```cpp
#include <fomapay/fomapay.hpp>

fomapay::Client client("sk_test_…");  // FomaPay Business › Développeurs

fomapay::PaymentParams params;
params.amount = 1000;
params.currency = fomapay::CDF;
params.order_id = "CMD-2026-001";
params.customer = {"Jean Marc", "+243810000000", "jean@example.com"};
params.payment_modes = {fomapay::FOMAPAY_DIRECT, fomapay::MOBILE_MONEY};
params.callback_url = "https://marchand.cd/api/callback";
params.metadata["custom_field"] = "valeur";

fomapay::Payment payment = client.create_payment(params);
// FomaPay Direct : affichez payment.qr_code à l'écran.

// Mobile Money : demande de débit (push USSD) sur le téléphone du client.
client.pay_with_mobile_money(payment.id, fomapay::AIRTEL_MONEY, "+243970000000");
```

### 3. Confirmer le paiement

Sur un terminal, interrogez l'état jusqu'à l'issue :

```cpp
while (payment.pending()) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    payment = client.retrieve_payment(payment.id);
}
if (payment.succeeded()) { /* imprimer le reçu */ }
```

Sur un serveur, vérifiez le webhook :

```cpp
// payload : le corps BRUT de la requête ; header : la valeur de X-FomaPay-Signature
try {
    fomapay::Event event = fomapay::webhook::construct_event(payload, header, "whsec_…");
    if (event.type == "payment.succeeded") livrer(event.data.order_id);
} catch (const fomapay::SignatureError&) {
    // répondre 400
}
```

`fomapay::webhook::verify(payload, header, secret)` renvoie simplement un booléen.

## Référence

| Appel | Rôle |
| --- | --- |
| `client.create_payment(params)` | Crée une demande de paiement |
| `client.retrieve_payment(id_ou_reference)` | Relit un paiement |
| `client.list_payments(limit, status)` | Derniers paiements de la boutique |
| `client.pay_with_mobile_money(id, operateur, telephone)` | Push USSD vers le client |
| `client.simulate_payment(id, "success" \| "failed")` | Force l'issue (sandbox uniquement) |
| `client.create_payout(params)` | Retrait vers Mobile Money |
| `client.balance()` | Solde par devise |
| `client.store()` | Boutique, niveau KYC et modes autorisés |

`create_transaction` est un alias de `create_payment`. Les montants sont des `double` en unités de la devise (`12.5` = 12,50 USD). Les appels sont bloquants : lancez-les hors du thread d'interface. Une même instance de `Client` peut servir depuis plusieurs threads.

## Erreurs

```cpp
try {
    client.create_payment(params);
} catch (const fomapay::ApiError& e) {      // l'API a répondu avec une erreur
    e.code();    // ex. ERROR_LEVEL_0_LIMIT_REACHED
    e.status();  // statut HTTP
} catch (const fomapay::NetworkError& e) {  // ERROR_NETWORK ou ERROR_TIMEOUT
}
```

`ApiError`, `NetworkError` et `SignatureError` héritent de `fomapay::Error` (`std::runtime_error`).

## Fiabilité

```cpp
fomapay::ClientOptions options;
options.timeout_ms = 10000;  // 30 s par défaut
options.max_retries = 3;     // 2 par défaut
fomapay::Client client("sk_live_…", options);
```

- **Nouvelles tentatives** avec attente exponentielle (0,5 s, 1 s… plafonnée à 8 s). Ne sont rejouées que les requêtes sans risque : les lectures, et les créations de paiement, qui portent toujours un en-tête `Idempotency-Key`. Un `429` est toujours rejoué en respectant `Retry-After`.
- **Idempotence** : renseignez `params.idempotency_key` (ex. le numéro de ticket) pour qu'un appel répété après une coupure renvoie le même paiement.
- **Retraits** : jamais rejoués automatiquement. Après une `NetworkError`, vérifiez l'état avant de relancer.

## Sans libcurl

Implémentez `fomapay::HttpTransport` (une seule méthode, `send`) au-dessus de la pile réseau de votre plateforme et passez-la dans `ClientOptions::transport`. Retirez alors `src/curl_transport.cpp` de votre build et fournissez `fomapay::internal::make_curl_transport()` renvoyant votre transport.

## Sandbox

Avec une clé `sk_test_…`, aucun argent ne circule. Pour un push Mobile Money, le numéro décide de l'issue : terminé par `0001` → refusé, par `0002` → jamais validé, sinon confirmé après ~3 s.

## Compiler et tester

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests d'intégration contre le sandbox (ignorés si la variable est absente) :

```bash
FOMAPAY_API_URL=http://localhost:8080 FOMAPAY_TEST_SECRET_KEY=sk_test_… ./build/fomapay_tests
```

Un exemple de caisse en ligne de commande se trouve dans [examples/pos.cpp](examples/pos.cpp).

Licence MIT.
