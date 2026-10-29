// Exercise actual FSP handlers and on-disk cache state; no game binaries needed.
#define _GNU_SOURCE
#include <assert.h>
#include "../runtime/services/fs.c"

void plat_write_prepare(uint64_t a, uint64_t n) { (void)a; (void)n; }
void test_session_file_read(const char *path) { (void)path; } // session behavior has its own fixture
void rt_fatal_trace(void) { abort(); }
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    Service *s = calloc(1, sizeof *s);
    s->name = name; s->handler = handler; s->state = state; return s;
}
void ipc_return_object(IpcResp *r, Service *s) { r->objects[r->nobjects++] = s; }
static void close_service(Service *s) { if (s->on_close) s->on_close(s); free(s); }
static IpcResp request(uint32_t cmd, void *data, uint32_t size) {
    IpcReq req = {.cmd = cmd, .data = data, .data_size = size};
    IpcResp resp = {0}; assert(fsp_save(&req, &resp)); return resp;
}
static IpcResp create(uint16_t index, int64_t size, int64_t journal) {
    uint8_t data[0x90] = {0};
    data[0x20] = 5; memcpy(data + 0x22, &index, 2); data[0x64] = 1;
    memcpy(data + 0x40, &size, 8); memcpy(data + 0x48, &journal, 8);
    return request(22, data, sizeof data);
}
static IpcResp mount(uint8_t type, uint16_t index, bool read_only) {
    uint8_t data[0x48] = {0}; data[0] = type == 4 ? 3 : 1;
    data[0x28] = type; memcpy(data + 0x2a, &index, 2);
    return request(read_only ? 53 : 51, data, sizeof data);
}
static int64_t read_list(Service *s, SaveInfo *info, size_t bytes) {
    IpcReq req = {.nb = 1, .b = {{(uintptr_t)info, bytes}}};
    IpcResp resp = {0}; s->handler(s, &req, &resp);
    assert(!resp.result && resp.data_size == 8); int64_t n; memcpy(&n, resp.data, 8); return n;
}
int main(int argc, char **argv) {
    assert(argc == 2); // Explicit project-local scratch root; never default to /tmp.
    char root[1024]; snprintf(root, sizeof root, "%s/save-check-XXXXXX", argv[1]);
    assert(mkdtemp(root)); setenv("LARECOMP_SAVE_DIR", root, 1);
    IpcResp r = request(62, NULL, 0); assert(!r.result && r.nobjects == 1);
    SaveInfo info[2]; assert(read_list(r.objects[0], info, sizeof info) == 0); close_service(r.objects[0]);
    assert(mount(5, 3, false).result == FS_TARGET_NOT_FOUND);
    uint8_t other[0x48] = {1};
    uint64_t botw = 0x01007ef00011e000ull;
    memcpy(other + 8, &botw, 8); other[0x28] = 1;
    assert(request(51, other, sizeof other).result == FS_TARGET_NOT_FOUND);
    assert(create(3, -1, 0).result);
    assert(create(3, INT64_MAX, 1).result == MAKE_RESULT(2, 30));
    assert(!create(3, 16000, 8000).result && !create(1, 32000, 8000).result);
    assert(create(3, 16, 8).result == FS_PATH_EXISTS);
    r = request(62, NULL, 0); assert(!r.result && r.nobjects == 1);
    Service *reader = r.objects[0];
    assert(!fs_create_cache_storage(2, 32000, 8000));
    assert(create(2, 32000, 8000).result == FS_PATH_EXISTS); // AM and FSP use one catalog.
    uint16_t index = 1; assert(!request(33, &index, 2).result);
    memset(info, 0xff, sizeof info);
    assert(read_list(reader, info, 0x5f) == 0 && info[0].bytes[0] == 0xff);
    assert(read_list(reader, info, 0x61) == 1);
    assert(info[0].bytes[8] == 1 && info[0].bytes[9] == 5);
    assert(*(uint16_t *)(info[0].bytes + 0x38) == 1 && info[1].bytes[0] == 0xff);
    assert(*(uint64_t *)(info[0].bytes + 0x28) == TOTK_TITLE_ID);
    assert(read_list(reader, info, sizeof info) == 1 && *(uint16_t *)(info[0].bytes + 0x38) == 3);
    assert(read_list(reader, info, sizeof info) == 0); close_service(reader);
    index = 3; r = request(34, &index, 2);
    assert(!r.result && r.data_size == 16 && *(int64_t *)r.data == 16000 && *(int64_t *)(r.data + 8) == 8000);
    Service *mounted[5];
    for (unsigned i = 0; i < 4; i++) {
        r = mount((uint8_t[]){1, 3, 4, 5}[i], 3, false);
        assert(!r.result && r.nobjects == 1); mounted[i] = r.objects[0];
        for (unsigned j = 0; j < i; j++)
            assert(strcmp(((HostFs *)mounted[i]->state)->root, ((HostFs *)mounted[j]->state)->root));
    }
    assert(request(33, &index, 2).result == FS_TARGET_LOCKED);
    r = mount(5, 3, true); assert(!r.result); mounted[4] = r.objects[0];
    uint8_t data[16] = {0}; const char *path = "/test";
    IpcReq req = {.cmd = 0, .data = data, .data_size = 16, .nx = 1, .x = {{(uintptr_t)path, 6}}};
    r = (IpcResp){0}; mounted[4]->handler(mounted[4], &req, &r); assert(r.result);
    r = (IpcResp){0}; mounted[3]->handler(mounted[3], &req, &r); assert(!r.result);
    req.x[0].addr = (uintptr_t)"/../metadata"; req.x[0].size = 13;
    req.cmd = 7; r = (IpcResp){0}; mounted[3]->handler(mounted[3], &req, &r); assert(r.result);
    for (unsigned i = 0; i < 5; i++) close_service(mounted[i]);
    assert(!request(33, &index, 2).result && request(34, &index, 2).result == FS_PATH_NOT_FOUND);
    assert(!rm_rf(root, false));
    puts("save storage tests passed: empty/snapshot/paged lists, capacities, duplicate create, isolated mounts, read-only and deletion");
}
