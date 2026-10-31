#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace shader_artifact_storage {
struct Stats {uint64_t stored,failed,deferred;};
// Background list entries may fill unused space, but cannot evict metadata
// needed by title/recent preparation or gameplay. Restore the producer's role
// on cancellation too; compile workers have a separate scheduling context.
bool exchange_background(bool background);
class BackgroundScope {
    bool previous;
public:
    explicit BackgroundScope(bool background):previous(exchange_background(background)){}
    ~BackgroundScope(){exchange_background(previous);}
    BackgroundScope(const BackgroundScope&)=delete;
    BackgroundScope& operator=(const BackgroundScope&)=delete;
};
// Capacity observed by the writer for its current namespace. This is only an
// admission hint for optional background repair, never a cache-validity test.
bool at_capacity();
bool enqueue(std::string path,std::vector<uint8_t> bytes);
void wait(const std::string& path);
void flush();
Stats stats();
}
