// RomFS image from a folder of game files (runtime/romfs_build.c). The game reads its data through one raw RomFS
// image (romfs.bin, runtime/services/fs.c); players bring their own game files, and the start screen builds the image
// from their romfs/ folder.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes the RomFS image of the folder `dir` to `out`. progress (may be NULL): bytes of file data written so far and
// the total, now and then. false = failed (why: a short message).
bool romfs_build(const char *dir, const char *out, void (*progress)(uint64_t done, uint64_t total), char *why,
                 size_t why_size);

// The same image from a list of files (relative paths "a/b/c" and sizes) whose data open_fd(ctx, index) hands over as
// a readable descriptor (closed here) - Android: straight from the picked folder, no copy of it first. The files
// keep(rel) picks are also written loose under keep_dir/rel (the few the runtime opens by path). keep may be NULL.
typedef struct {
    const char *path;
    uint64_t size;
} RomfsEntry;
bool romfs_build_entries(const RomfsEntry *entries, int n, int (*open_fd)(void *ctx, int index), void *ctx,
                         const char *out, const char *keep_dir, bool (*keep)(const char *rel),
                         void (*progress)(uint64_t done, uint64_t total), char *why, size_t why_size);

// Whether the image at `path` is whole: its header, and the file as long as the tables the header points at (they
// end the image, so a copy cut short loses them while its header still looks right). false = why says what's wrong.
bool romfs_image_ok(const char *path, char *why, size_t why_size);

// Bytes of file data under dir (the image is about this plus its tables).
uint64_t romfs_dir_bytes(const char *dir);

// Whether a romfs/ goes with the game code beside it (version: exefs/main's, 100 / 160 / 182), from its files'
// relative paths and the first bytes of its System/Version.txt ("" without one): ROMFS_SET_OK; ROMFS_SET_OTHER_VERSION
// - Version.txt names another version; ROMFS_SET_MISSING - fewer files than that version's whole romfs. More files
// pass (mods). why: what was found, for the setup's failure line.
enum { ROMFS_SET_OK, ROMFS_SET_OTHER_VERSION, ROMFS_SET_MISSING };
int romfs_check_set(int version, const char *const *paths, int n, const char *version_txt, char *why, size_t why_size);
// The same for a folder on disk (its files as the image takes them: hidden ones left out)
int romfs_check_dir(int version, const char *dir, char *why, size_t why_size);

// Copies the folder tree src to dst (hard links where the file system allows: no second copy of 6 GB). progress as
// above. false = failed.
bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total));

#ifdef __cplusplus
}
#endif
