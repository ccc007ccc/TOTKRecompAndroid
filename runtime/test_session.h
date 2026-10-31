#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "diagnostics_build.h"

// Explicit developer test sessions. Normal launches perform no control-file I/O.
#if TOTK_DIAGNOSTICS
bool test_session_enabled(void);
bool test_session_timed(void); // 60 FPS benchmark: sample on readiness, then quit after the requested duration
bool test_session_should_exit(void); // event thread: result/status are already flushed before this becomes true
void test_session_backgrounded(void); // timed measurements stop if the game leaves the foreground
int test_session_save_slot(void); // -1 unless direct loading was explicitly requested
void test_session_file_read(const char *path); // stop menu input on actual progress-save reads, not resource preloads
int test_session_target_fps(void); // 0: normal setting; explicit test override never writes the player's ini
const char *test_session_directory(void);
void test_session_poll(void); // SDL/event thread, also while the renderer is paused
void test_session_frame(uint64_t frame, bool scene, void (*gpu_idle)(void));
uint64_t test_session_buttons(uint64_t frame);
int32_t test_session_camera_x(void); // optional constant turn, only during a timed sample
int32_t test_session_move_y(void); // optional forward movement, only during a timed sample
bool test_session_take_gpu_request(void);
bool test_session_take_shot_request(void);
void test_session_end(void); // release the renderer before guest teardown/reboot
void test_session_manual(bool resume); // event thread: works while rendering is paused
#else
static inline bool test_session_enabled(void) { return false; }
static inline bool test_session_timed(void) { return false; }
static inline bool test_session_should_exit(void) { return false; }
static inline void test_session_backgrounded(void) {}
static inline int test_session_save_slot(void) { return -1; }
static inline void test_session_file_read(const char *path) { (void)path; }
static inline int test_session_target_fps(void) { return 0; }
static inline const char *test_session_directory(void) { return ""; }
static inline void test_session_poll(void) {}
static inline void test_session_frame(uint64_t frame, bool scene, void (*gpu_idle)(void)) { (void)frame; (void)scene; (void)gpu_idle; }
static inline uint64_t test_session_buttons(uint64_t frame) { (void)frame; return 0; }
static inline int32_t test_session_camera_x(void) { return 0; }
static inline int32_t test_session_move_y(void) { return 0; }
static inline bool test_session_take_gpu_request(void) { return false; }
static inline bool test_session_take_shot_request(void) { return false; }
static inline void test_session_end(void) {}
static inline void test_session_manual(bool resume) { (void)resume; }
#endif
