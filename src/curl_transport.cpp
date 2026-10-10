#include <curl/curl.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

#include "internal.hpp"

namespace fomapay::internal {

namespace {

std::size_t on_body(char* data, std::size_t size, std::size_t count, void* target) {
    static_cast<std::string*>(target)->append(data, size * count);
    return size * count;
}

std::size_t on_header(char* data, std::size_t size, std::size_t count, void* target) {
    static const char name[] = "retry-after:";
    const std::size_t length = size * count;
    if (length > sizeof name - 1) {
        bool match = true;
        for (std::size_t i = 0; i < sizeof name - 1 && match; ++i) {
            const char c = data[i];
            match = (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) == name[i];
        }
        if (match) {
            const std::string value(data + sizeof name - 1, length - (sizeof name - 1));
            *static_cast<long*>(target) = std::strtol(value.c_str(), nullptr, 10);
        }
    }
    return length;
}

class CurlTransport : public HttpTransport {
public:
    CurlTransport() {
        static std::once_flag once;
        std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    // Une poignée par requête : aucun état partagé, donc utilisable depuis plusieurs threads.
    HttpResponse send(const HttpRequest& request) override {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw NetworkError("ERROR_NETWORK", "Impossible d'initialiser libcurl.");
        }

        HttpResponse response;
        curl_slist* headers = nullptr;
        for (const auto& header : request.headers) {
            headers = curl_slist_append(headers, (header.first + ": " + header.second).c_str());
        }
        char error[CURL_ERROR_SIZE] = "";

        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, request.timeout_ms);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.retry_after);
        if (!request.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        }

        const CURLcode result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (result == CURLE_OPERATION_TIMEDOUT) {
            throw NetworkError("ERROR_TIMEOUT", "FomaPay n'a pas répondu dans le délai imparti.");
        }
        if (result != CURLE_OK) {
            throw NetworkError("ERROR_NETWORK",
                               std::string("Impossible de joindre FomaPay : ") + (error[0] ? error : curl_easy_strerror(result)));
        }
        response.status = static_cast<int>(status);
        return response;
    }
};

}  // namespace

std::shared_ptr<HttpTransport> make_curl_transport() { return std::make_shared<CurlTransport>(); }

}  // namespace fomapay::internal
