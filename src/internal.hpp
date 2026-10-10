// Éléments partagés entre les fichiers sources de libfomapay (non installés).
#ifndef FOMAPAY_INTERNAL_HPP
#define FOMAPAY_INTERNAL_HPP

#include <memory>

#include "fomapay/fomapay.hpp"
#include "json.hpp"

namespace fomapay::internal {

Payment to_payment(const json::Value& value);

/// Transport par défaut, basé sur libcurl.
std::shared_ptr<HttpTransport> make_curl_transport();

}  // namespace fomapay::internal

#endif  // FOMAPAY_INTERNAL_HPP
