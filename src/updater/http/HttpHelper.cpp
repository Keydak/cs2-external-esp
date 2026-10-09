#include "HttpHelper.hpp"

int HttpHelper::Get(std::string url, json& response) {
	return GetInstance().GetImpl(url, response);
}

int HttpHelper::GetRaw(std::string url, std::string& response) {
	return GetInstance().GetRawImpl(url, response);
}

int HttpHelper::Post(std::string url, json body, json& response) {
	return GetInstance().PostImpl(url, body, response);
}

int HttpHelper::GetImpl(std::string url, json& response) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    std::string response_string;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // Accept every supported compression (gzip, deflate), json shrinks ~8x
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cs2-external-esp"); // The GitHub API refuses requests without one

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOGF(WARNING, "Curl returned a non successfull status code {}", (int)res);
        return -1;
    }

    try {
        response = json::parse(response_string);
    }
    catch (...) {
        return -2; // parse error
    }

    return static_cast<int>(http_code);
}

int HttpHelper::GetRawImpl(std::string url, std::string& response) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    response.clear();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cs2-external-esp");

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return -1;

    return static_cast<int>(http_code);
}

int HttpHelper::GetRange(const std::string& url, uint64_t first, uint64_t last, std::string& response) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return -1;

    response.clear();
    auto range = std::format("{}-{}", first, last);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cs2-external-esp");

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return -1;

    return static_cast<int>(http_code);
}

int HttpHelper::Download(const std::string& url, const std::filesystem::path& path, uint64_t max_size, const Progress& progress) {
    struct Transfer {
        std::ofstream file;
        uint64_t written = 0;
        uint64_t max_size = 0;
        const Progress* progress = nullptr;
    } transfer;

    transfer.file.open(path, std::ios::binary | std::ios::trunc);
    if (!transfer.file.good())
        return -1;
    transfer.max_size = max_size;
    transfer.progress = &progress;

    CURL* curl = curl_easy_init();
    if (!curl)
        return -1;

    auto write = [](char* data, size_t size, size_t count, void* user) -> size_t {
        auto t = static_cast<Transfer*>(user);
        size_t bytes = size * count;
        if (t->written + bytes > t->max_size)
            return 0;   // Larger than allowed, stops the transfer

        t->file.write(data, static_cast<std::streamsize>(bytes));
        t->written += bytes;
        return t->file.good() ? bytes : 0;
    };

    auto info = [](void* user, curl_off_t total, curl_off_t done, curl_off_t, curl_off_t) -> int {
        auto t = static_cast<Transfer*>(user);
        if (total > 0 && static_cast<uint64_t>(total) > t->max_size)
            return 1;
        if (*t->progress && !(*t->progress)(static_cast<uint64_t>(done), static_cast<uint64_t>(total)))
            return 1;
        return 0;
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<curl_write_callback>(write));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, static_cast<curl_xferinfo_callback>(info));
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // Under 1 KB/s for 30 s: given up
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cs2-external-esp");

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    transfer.file.close();
    if (res != CURLE_OK)
        return -1;

    return static_cast<int>(http_code);
}

int HttpHelper::PostImpl(std::string url, json body, json& response) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    std::string response_string;
    std::string body_string = body.dump();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_string.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_string.size());

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

    // Set JSON header
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return -1;

    try {
        response = json::parse(response_string);
    }
    catch (...) {
        return -2; // parse error
    }

    return static_cast<int>(http_code);
}

size_t HttpHelper::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
	size_t total_size = size * nmemb;
	std::string* str = static_cast<std::string*>(userp);
	str->append(static_cast<char*>(contents), total_size);
	return total_size;
}