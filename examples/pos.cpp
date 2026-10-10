// Exemple : encaissement Mobile Money depuis un terminal de caisse.
//
//   FOMAPAY_SECRET_KEY=sk_test_… ./fomapay_pos 5000 CDF airtel_money +243970000000
//
// FOMAPAY_API_URL permet de viser un backend local (http://localhost:8080).
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include <fomapay/fomapay.hpp>

int main(int argc, char** argv) {
    const char* key = std::getenv("FOMAPAY_SECRET_KEY");
    if (argc < 5 || !key) {
        std::cerr << "usage : FOMAPAY_SECRET_KEY=sk_… " << argv[0] << " <montant> <USD|CDF> <operateur> <telephone>\n";
        return 2;
    }

    try {
        fomapay::ClientOptions options;
        if (const char* url = std::getenv("FOMAPAY_API_URL")) {
            options.base_url = url;
        }
        const fomapay::Client client(key, options);

        fomapay::PaymentParams params;
        params.amount = std::atof(argv[1]);
        params.currency = argv[2];
        params.description = "Vente en caisse";
        fomapay::Payment payment = client.create_payment(params);
        std::cout << "Paiement " << payment.reference << " cree. Demande envoyee au " << argv[4] << "...\n";

        payment = client.pay_with_mobile_money(payment.id, argv[3], argv[4]);
        while (payment.pending()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            payment = client.retrieve_payment(payment.id);
        }

        if (payment.succeeded()) {
            std::cout << "PAYE : " << payment.amount << " " << payment.currency << " (net " << payment.net << ")\n";
            return 0;
        }
        std::cout << "ECHEC : " << payment.failure_reason << "\n";
        return 1;
    } catch (const fomapay::NetworkError& e) {
        std::cerr << "Connexion impossible, reessayez : " << e.what() << "\n";
    } catch (const fomapay::Error& e) {
        std::cerr << "Refuse par FomaPay : " << e.what() << "\n";
    }
    return 1;
}
