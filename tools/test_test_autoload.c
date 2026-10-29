// Exercise the real session initialization, save clone and menu input cutoff.
#define _POSIX_C_SOURCE 200809L
#include "../runtime/test_session.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint64_t plat_now_ns(void){return 1000000000;}
void plat_sleep_until_ns(uint64_t when){(void)when;}
static void directory(const char* path) {assert(!mkdir(path,0700));}
int main(int argc,char** argv) {
    assert(argc==2 && !chdir(argv[1]));
    directory("seed");directory("seed/stores");
    directory("seed/stores/1_1_0_0_0100f2c0115b6000_fixture");
    directory("seed/stores/1_1_0_0_0100f2c0115b6000_fixture/files");
    directory("seed/stores/1_1_0_0_0100f2c0115b6000_fixture/files/slot_02");
    const char* progress="seed/stores/1_1_0_0_0100f2c0115b6000_fixture/files/slot_02/progress.sav";
    FILE* file=fopen(progress,"wb");assert(file && fputs("synthetic save",file)>=0 && !fclose(file));
    file=fopen("seed/stores/1_1_0_0_0100f2c0115b6000_fixture/metadata","wb");
    assert(file && fputs("synthetic metadata",file)>=0 && !fclose(file));
    setenv("LARECOMP_TEST_SESSION","session",1);
    setenv("LARECOMP_TEST_SAVE_SEED","seed",1);
    setenv("LARECOMP_SAVE_DIR","session/save-test",1);
    setenv("LARECOMP_TEST_SAVE_SLOT","2",1);
    setenv("LARECOMP_TEST_DURATION_SECONDS","15",1);
    unsetenv("LARECOMP_TEST_AUTOLOAD");
    assert(test_session_enabled() && test_session_save_slot()==2 && test_session_timed());
    assert(!test_session_buttons(119) && test_session_buttons(120)==1 && test_session_buttons(121)==1);
    assert(!test_session_buttons(122) && test_session_buttons(180)==1);
    assert(!access("session/save-test/stores/1_1_0_0_0100f2c0115b6000_fixture/metadata",R_OK));
    test_session_file_read(NULL);
    test_session_file_read(progress); // original save, not this session's copy
    test_session_file_read("session/save-test/stores/fixture/files/slot_01/progress.sav");
    test_session_file_read("session/save-test/stores/fixture/files/slot_02/caption.sav");
    test_session_file_read("session/save-test-other/stores/fixture/files/slot_02/progress.sav");
    test_session_file_read("Sequence/OnLoadGameSaveData.module.ainb"); // eagerly preloaded before the title
    assert(test_session_buttons(240)==1);
    test_session_file_read("session/save-test/stores/fixture/files/slot_02/progress.sav");
    test_session_file_read("session/save-test/stores/fixture/files/slot_02/progress.sav");
    for(unsigned frame=181;frame<1000;++frame)assert(!test_session_buttons(frame));
    assert(test_session_target_fps()==60 && !test_session_should_exit());
    file=fopen(progress,"rb");char data[32]={0};assert(file);assert(fread(data,1,sizeof data,file)==14);
    assert(!strcmp(data,"synthetic save") && !fclose(file));
    puts("PASS: explicit slot keeps menu confirmation, clones the save and metadata, stops automatic input before gameplay");
}
