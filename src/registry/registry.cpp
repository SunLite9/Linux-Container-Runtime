#include "registry.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace cr::registry {

namespace {

namespace stdfs = std::filesystem;
using json = nlohmann::json;

constexpr const char* kAuthUrl = "https://auth.docker.io/token";
constexpr const char* kRegistryUrl = "https://registry-1.docker.io";

constexpr const char* kManifestAcceptHeader =
    "Accept: application/vnd.docker.distribution.manifest.v2+json,"
    "application/vnd.docker.distribution.manifest.list.v2+json,"
    "application/vnd.oci.image.manifest.v1+json,"
    "application/vnd.oci.image.index.v1+json";

size_t writeToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t writeToFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    return fwrite(ptr, size, nmemb, static_cast<FILE*>(userdata));
}

CURLcode performGet(CURL* curl, const std::string& url, const std::vector<std::string>& headers,
                     struct curl_slist** headerListOut, long* httpCodeOut) {
    struct curl_slist* headerList = nullptr;
    for (const auto& h : headers) {
        headerList = curl_slist_append(headerList, h.c_str());
    }
    *headerListOut = headerList;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "container-runtime/0.1");

    const CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, httpCodeOut);
    return res;
}

std::string httpGetString(const std::string& url, const std::vector<std::string>& headers) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    struct curl_slist* headerList = nullptr;
    long httpCode = 0;
    const CURLcode res = performGet(curl, url, headers, &headerList, &httpCode);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("GET " + url + " failed: " + curl_easy_strerror(res));
    }
    if (httpCode >= 400) {
        throw std::runtime_error("GET " + url + " returned HTTP " + std::to_string(httpCode));
    }
    return response;
}

void httpDownloadToFile(const std::string& url, const std::vector<std::string>& headers,
                         const std::string& destPath) {
    FILE* file = fopen(destPath.c_str(), "wb");
    if (!file) {
        throw std::runtime_error("fopen failed: " + destPath);
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        throw std::runtime_error("curl_easy_init failed");
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

    struct curl_slist* headerList = nullptr;
    long httpCode = 0;
    const CURLcode res = performGet(curl, url, headers, &headerList, &httpCode);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    fclose(file);

    if (res != CURLE_OK || httpCode >= 400) {
        stdfs::remove(destPath);
        throw std::runtime_error("GET " + url + " failed (curl=" + std::to_string(res) +
                                  ", http=" + std::to_string(httpCode) + ")");
    }
}

struct ImageRef {
    std::string repository;
    std::string tag;
};

ImageRef parseImageRef(const std::string& imageRef) {
    std::string name = imageRef;
    std::string tag = "latest";

    const auto colon = name.find_last_of(':');
    if (colon != std::string::npos) {
        tag = name.substr(colon + 1);
        name = name.substr(0, colon);
    }
    if (name.find('/') == std::string::npos) {
        name = "library/" + name;
    }
    return {name, tag};
}

std::string fetchToken(const std::string& repository) {
    const std::string url =
        std::string(kAuthUrl) + "?service=registry.docker.io&scope=repository:" + repository + ":pull";
    const json body = json::parse(httpGetString(url, {}));
    return body.at("token").get<std::string>();
}

json fetchManifest(const std::string& repository, const std::string& reference, const std::string& token) {
    const std::string url = std::string(kRegistryUrl) + "/v2/" + repository + "/manifests/" + reference;
    const json manifest = json::parse(
        httpGetString(url, {"Authorization: Bearer " + token, kManifestAcceptHeader}));

    const std::string mediaType = manifest.value("mediaType", "");
    const bool isList = mediaType == "application/vnd.docker.distribution.manifest.list.v2+json" ||
                         mediaType == "application/vnd.oci.image.index.v1+json";
    if (!isList) {
        return manifest;
    }

    for (const auto& candidate : manifest.at("manifests")) {
        const auto& platform = candidate.at("platform");
        if (platform.value("architecture", "") == "amd64" && platform.value("os", "") == "linux") {
            return fetchManifest(repository, candidate.at("digest").get<std::string>(), token);
        }
    }
    throw std::runtime_error("no linux/amd64 manifest found for " + repository + ":" + reference);
}

std::string sanitize(const std::string& repository) {
    std::string s = repository;
    for (char& c : s) {
        if (c == '/') {
            c = '_';
        }
    }
    return s;
}

} // namespace

PulledImage pull(const std::string& imageRef, const std::string& cacheDir) {
    const ImageRef ref = parseImageRef(imageRef);
    const std::string imageCacheDir = cacheDir + "/" + sanitize(ref.repository) + "/" + ref.tag;
    stdfs::create_directories(imageCacheDir);

    const std::string token = fetchToken(ref.repository);
    const json manifest = fetchManifest(ref.repository, ref.tag, token);

    std::vector<std::string> layerDirsBaseToTop;
    for (const auto& layer : manifest.at("layers")) {
        const std::string digest = layer.at("digest").get<std::string>();
        const std::string shortId = digest.substr(digest.find(':') + 1, 16);
        const std::string layerDir = imageCacheDir + "/" + shortId;

        const bool alreadyExtracted = stdfs::exists(layerDir) && !stdfs::is_empty(layerDir);
        if (!alreadyExtracted) {
            const std::string blobUrl = std::string(kRegistryUrl) + "/v2/" + ref.repository + "/blobs/" + digest;
            const std::string tarPath = "/tmp/container-runtime-layer-" + shortId + ".tar.gz";
            httpDownloadToFile(blobUrl, {"Authorization: Bearer " + token}, tarPath);

            stdfs::create_directories(layerDir);
            const std::string extractCmd = "tar -xzf " + tarPath + " -C " + layerDir;
            if (std::system(extractCmd.c_str()) != 0) {
                throw std::runtime_error("tar extraction failed for layer " + shortId);
            }
            stdfs::remove(tarPath);
        }
        layerDirsBaseToTop.push_back(layerDir);
    }

    PulledImage result;
    result.layerDirs.assign(layerDirsBaseToTop.rbegin(), layerDirsBaseToTop.rend());
    return result;
}

} // namespace cr::registry
