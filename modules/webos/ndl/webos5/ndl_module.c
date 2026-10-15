#include "ss4s/modapi.h"
#include "ndl_common.h"

#include <stdlib.h>
#include <assert.h>

pthread_mutex_t SS4S_NDL_webOS5_Lock = PTHREAD_MUTEX_INITIALIZER;
bool SS4S_NDL_webOS5_Initialized = false;
static bool SS4S_NDL_webOS5_InitAttempted = false;
SS4S_LoggingFunction *SS4S_NDL_webOS5_Log = NULL;
const SS4S_LibraryContext *SS4S_NDL_webOS5_Lib = NULL;

SS4S_EXPORTED bool SS4S_ModuleOpen_NDL_WEBOS5(SS4S_Module *module, const SS4S_LibraryContext *context) {
    (void) context;
    SS4S_NDL_webOS5_Lib = context;
    SS4S_NDL_webOS5_Log = context->Log;
    assert(SS4S_NDL_webOS5_Log != NULL);
    module->Name = "ndl-webos5";
    module->PlayerDriver = &SS4S_NDL_webOS5_PlayerDriver;
    module->AudioDriver = &SS4S_NDL_webOS5_AudioDriver;
    module->VideoDriver = &SS4S_NDL_webOS5_VideoDriver;
    return true;
}

int SS4S_NDL_webOS5_EnsureInit(void) {
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    if (SS4S_NDL_webOS5_Initialized) {
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return 0;
    }
    /* Audio and video PostInit both enter here. A failed NDL_DirectMediaInit
     * still leaves libNDL in a state where the next call jumps to 0x8d10
     * from GetPmLogContext (C1 crash report, SIGSEGV). */
    if (SS4S_NDL_webOS5_InitAttempted) {
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return -1;
    }
    SS4S_NDL_webOS5_InitAttempted = true;
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);

    int ret = NDL_DirectMediaInit(getenv("APPID"), NULL);
    if (ret == 0) {
        pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
        SS4S_NDL_webOS5_Initialized = true;
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Driver init.");
        return 0;
    }
    const char *error = NDL_DirectMediaGetError();
    SS4S_NDL_webOS5_Log(SS4S_LogLevelError, "NDL", "Failed to init: ret=%d, error=%s", ret,
                        error != NULL ? error : "");
    return ret;
}

int SS4S_NDL_webOS5_Driver_PostInit(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    /* Do not call NDL_DirectMediaInit here. On webOS 25 it runs ~200 ms after
     * launch, before the app is foreground, and returns -1. A failed init must
     * not be repeated (the next call SIGSEGVs in GetPmLogContext), so an early
     * failure permanently disables video. The first init happens in LoadMedia,
     * once a stream is actually starting. */
    return 0;
}

void SS4S_NDL_webOS5_Driver_Quit() {
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    if (!SS4S_NDL_webOS5_Initialized) {
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return;
    }
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Driver quit.");
    NDL_DirectMediaQuit();
    SS4S_NDL_webOS5_Initialized = false;
    SS4S_NDL_webOS5_InitAttempted = false;
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
}
