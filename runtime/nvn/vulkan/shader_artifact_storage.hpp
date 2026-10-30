#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace shader_artifact_storage {
struct Stats {uint64_t stored,failed;};
bool enqueue(std::string path,std::vector<uint8_t> bytes);
void wait(const std::string& path);
void flush();
Stats stats();
}
