#pragma once
#include <stdbool.h>
#include <stdint.h>

// Explicit developer test sessions. Normal launches perform no control-file I/O.
bool test_session_enabled(void);
int test_session_save_slot(void); // -1 unless direct loading was explicitly requested
const char *test_session_directory(void);
void test_session_poll(void); // SDL/event thread, also while the renderer is paused
void test_session_frame(uint64_t frame, bool scene, void (*gpu_idle)(void));
uint64_t test_session_buttons(uint64_t frame);
bool test_session_take_gpu_request(void);
bool test_session_take_shot_request(void);
void test_session_end(void); // release the renderer before guest teardown/reboot
void test_session_manual(bool resume); // event thread: works while rendering is paused
