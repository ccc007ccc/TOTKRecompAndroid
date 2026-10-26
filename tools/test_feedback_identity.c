#include "../runtime/nvn/vulkan/feedback_identity.h"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
int main(void) {
    CHECK(feedback_identity_matches(100, 1, 100, 1)); // original allocation
    CHECK(!feedback_identity_matches(100, 1, 200, 2)); // resize / replaced source
    CHECK(!feedback_identity_matches(100, 1, 100, 2)); // Vulkan recycled the old handle
    CHECK(!feedback_identity_matches(100, 1, 200, 1)); // copy-alias image swap
    CHECK(!feedback_identity_matches(0, 0, 0, 0)); // no initialized image
    CHECK(feedback_identity_matches(200, 2, 200, 2)); // refreshed snapshot
    return 0;
}
