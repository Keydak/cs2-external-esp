#pragma once

#ifndef CURL_STATICLIB
#define CURL_STATICLIB
#endif

#include <curl/curl.h>
#pragma comment(lib, "libcurl.lib")
#pragma comment(lib, "zlib.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

using json = nlohmann::json;

class HttpHelper {
public:
    ~HttpHelper() = default;
    HttpHelper(const HttpHelper&) = delete;
    HttpHelper(HttpHelper&&) = delete;
    HttpHelper& operator=(const HttpHelper&) = delete;
    HttpHelper& operator=(HttpHelper&&) = delete;

    static int Get(std::string url, json& response);
    static int GetRaw(std::string url, std::string& response); // Body as is, for files

    // A file of any size into path. progress(done, total) may return false to stop, total is 0 while unknown
    using Progress = std::function<bool(uint64_t done, uint64_t total)>;
    static int Download(const std::string& url, const std::filesystem::path& path, uint64_t max_size, const Progress& progress);

    // Bytes first to last of a file (206 when the server sent only those)
    static int GetRange(const std::string& url, uint64_t first, uint64_t last, std::string& response);
    static int Post(std::string url, json body, json& response);
private:
    HttpHelper() {};

    static HttpHelper& GetInstance()
    {
        static HttpHelper i{};
        return i;
    }

    int GetImpl(std::string url, json& response);
    int GetRawImpl(std::string url, std::string& response);
    int PostImpl(std::string url, json body, json& response);

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
};

