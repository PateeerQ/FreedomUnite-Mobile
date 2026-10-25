// FreedomUniteRecomp launcher -- PNG/JPEG into RGBA (WIC on Windows).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

bool image_load(const std::string &path, std::vector<uint8_t> &rgba, int &w, int &h);
bool image_save_png(const std::string &path, const uint8_t *rgba, int w, int h);
