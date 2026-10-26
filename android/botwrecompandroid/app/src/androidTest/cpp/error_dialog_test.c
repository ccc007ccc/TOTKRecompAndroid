#include <dlfcn.h>
#include <jni.h>

JNIEXPORT void JNICALL
Java_com_botwrecompandroid_RuntimeErrorDialogCheck_raiseRuntimeError(JNIEnv *env, jclass cls) {
    (void)cls;
    void *library = dlopen("libmain.so", RTLD_NOW | RTLD_NOLOAD);
    void (*fatal)(const char *, ...) = library ? dlsym(library, "rt_fatal_message") : NULL;
    if (!fatal) {
        jclass error = (*env)->FindClass(env, "java/lang/IllegalStateException");
        (*env)->ThrowNew(env, error, "Runtime error entry point unavailable");
        return;
    }
    fatal("Error-dialog verification: intentional native runtime check.\nCopy and OK must keep the app open.\n");
}
