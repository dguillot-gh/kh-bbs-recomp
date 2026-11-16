// Android entry for Birth by Sleep (2026-11-16): runs BBS's main() (host/main.cpp, compiled as bbs_main)
// on a big-stack game thread, hands the SurfaceView's ANativeWindow to the Vulkan GE backend, routes
// stdout/stderr to logcat (tag "BBS") and maps touch to PSP buttons.
#include "ge_gpu_backend.hpp"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int bbs_main(int argc, char **argv);
extern std::atomic<std::uint32_t> g_bbs_pad_buttons;

namespace {
std::atomic<bool> g_started{false};
ANativeWindow *g_window = nullptr;

void *log_pump(void *arg) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    std::string line;
    char buffer[1024];
    for (;;) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buffer[i] == '\n' || line.size() > 900) {
                __android_log_write(ANDROID_LOG_INFO, "BBS", line.c_str());
                line.clear();
                if (buffer[i] == '\n') continue;
            }
            line.push_back(buffer[i]);
        }
    }
    return nullptr;
}

void redirect_stdio_to_logcat() {
    static bool done = false;
    if (done) return;
    done = true;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    pthread_t thread;
    pthread_create(&thread, nullptr, log_pump, reinterpret_cast<void *>(static_cast<intptr_t>(fds[0])));
    pthread_detach(thread);
}

struct GameArgs {
    std::vector<std::string> args;
};

void *game_thread(void *raw) {
    auto *game = static_cast<GameArgs *>(raw);
    std::vector<char *> argv;
    for (auto &a : game->args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::cout << "[bbs-android] starting game thread" << std::endl;
    const int code = bbs_main(static_cast<int>(game->args.size()), argv.data());
    std::cout << "[bbs-android] bbs_main returned " << code << std::endl;
    std::cout.flush();
    delete game;
    return nullptr;
}
} // namespace

extern "C" JNIEXPORT void JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeSetSurface(JNIEnv *env, jclass,
                                                                                        jobject surface) {
    ANativeWindow *window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    vcs::ge_gpu_backend_set_native_window(window);
    if (g_window != nullptr) ANativeWindow_release(g_window);
    g_window = window;
}

extern "C" JNIEXPORT void JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeSetButtons(JNIEnv *, jclass, jint buttons) {
    g_bbs_pad_buttons.store(static_cast<std::uint32_t>(buttons), std::memory_order_relaxed);
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeStart(JNIEnv *env, jclass,
                                                                                    jstring files_dir,
                                                                                    jstring eboot, jstring disc,
                                                                                    jlong max_dispatches) {
    if (g_started.exchange(true)) return JNI_TRUE;
    redirect_stdio_to_logcat();
    auto str = [env](jstring s) {
        const char *c = env->GetStringUTFChars(s, nullptr);
        std::string out = c;
        env->ReleaseStringUTFChars(s, c);
        return out;
    };
    const std::string dir = str(files_dir);
    // vcs::initialize_vcs_configuration reads VCSNative.ini beside argv[0]; the Java side writes it to files/.
    setenv("PSPRECOMP_CONFIG", (dir + "/VCSNative.ini").c_str(), 0);
    auto *game = new GameArgs{{dir + "/bbs", str(eboot), str(disc), std::to_string(max_dispatches)}};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // clang frames of the huge generated functions are large (the Windows exe uses a 256 MB stack).
    pthread_attr_setstacksize(&attr, 256u * 1024u * 1024u);
    pthread_t thread;
    const int rc = pthread_create(&thread, &attr, game_thread, game);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "BBS", "pthread_create failed: %d", rc);
        delete game;
        return JNI_FALSE;
    }
    pthread_detach(thread);
    return JNI_TRUE;
}
