// FreedomUniteRecomp launcher -- see http.h.
#include "http.h"
#include "config.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

static std::wstring W(const std::string &s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), (int)w.size());
    return w;
}

static bool request(const std::string &url, const std::string *form, std::string &body, std::string &err) {
    body.clear();
    std::wstring wurl = W(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2048;
    wchar_t extra[2048];
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { err = "bad URL"; return false; }
    std::wstring object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);

    const std::wstring agent = W("FreedomUniteRecomp-Launcher/" LAUNCHER_VERSION);
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = "WinHTTP is not available"; return false; }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    HINTERNET c = WinHttpConnect(s, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, form ? L"POST" : L"GET", object.c_str(), NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : NULL;
    bool ok = false;
    const wchar_t *headers = form ? L"Accept: application/json, */*\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                                  : L"Accept: application/json, */*\r\n";
    const DWORD flen = form ? (DWORD)form->size() : 0;
    if (r && WinHttpSendRequest(r, headers, (DWORD)-1L, form ? (LPVOID)form->data() : WINHTTP_NO_REQUEST_DATA, flen, flen, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &len, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got) || !got) break;
            body.append(chunk.data(), got);
        }
        ok = status == 200;
        if (!ok) err = "HTTP " + std::to_string(status);
    } else {
        err = "no connection (error " + std::to_string(GetLastError()) + ")";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

bool http_get(const std::string &url, std::string &body, std::string &err) { return request(url, nullptr, body, err); }
bool http_post(const std::string &url, const std::string &form, std::string &body, std::string &err) {
    return request(url, &form, body, err);
}

#else

bool http_get(const std::string &, std::string &body, std::string &err) {
    body.clear();
    err = "online features are Windows-only for now";
    return false;
}
bool http_post(const std::string &url, const std::string &, std::string &body, std::string &err) {
    return http_get(url, body, err);
}

#endif
