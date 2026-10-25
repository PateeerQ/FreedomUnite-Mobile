// FreedomUniteRecomp launcher -- HTTPS GET (WinHTTP on Windows).
#pragma once

#include <string>

// Fetches `url`. True with the body when the server answered 200; otherwise
// false, and `err` says why (the HTTP status, or the connection's error).
bool http_get(const std::string &url, std::string &body, std::string &err);
// The same as a form POST (application/x-www-form-urlencoded).
bool http_post(const std::string &url, const std::string &form, std::string &body, std::string &err);
