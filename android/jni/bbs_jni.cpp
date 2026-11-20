// Android entry for Birth by Sleep (2026-11-16): runs BBS's main() (host/main.cpp, compiled as bbs_main)
// on a big-stack game thread, hands the SurfaceView's ANativeWindow to the Vulkan GE backend, routes
// stdout/stderr to logcat (tag "BBS") and maps touch to PSP buttons.
#include "ge_gpu_backend.hpp"
#include "texture_replacer.hpp"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

int bbs_main(int argc, char **argv);
extern std::atomic<std::uint32_t> g_bbs_pad_buttons;
extern std::atomic<std::uint32_t> g_bbs_pad_analog;
extern std::atomic<int> g_bbs_state_request;
extern std::atomic<int> g_bbs_state_result;
extern std::mutex g_bbs_state_mutex;
extern std::string g_bbs_state_path;
extern std::atomic<bool> g_bbs_paused;
extern std::atomic<std::uint64_t> g_bbs_flip_count;
extern std::atomic<bool> g_bbs_60fps;
extern std::atomic<std::uint32_t> g_bbs_boss_checkpoints;
extern std::atomic<int> g_bbs_character;

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

// Sticks as 0..255 (128 = centre, y grows downward like the PSP).
extern "C" JNIEXPORT void JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeSetAnalog(JNIEnv *, jclass, jint lx,
                                                                                   jint ly, jint rx, jint ry) {
    auto c = [](jint v) { return static_cast<std::uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    g_bbs_pad_analog.store(c(lx) | (c(ly) << 8u) | (c(rx) << 16u) | (c(ry) << 24u), std::memory_order_relaxed);
}

// Save states (2026-11-18): op 1 = save, 2 = load, carried out by the game thread at its next vblank wait.
extern "C" JNIEXPORT void JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeRequestState(JNIEnv *env, jclass, jint op,
                                                                                       jstring path) {
    const char *c = env->GetStringUTFChars(path, nullptr);
    {
        std::lock_guard<std::mutex> lock(g_bbs_state_mutex);
        g_bbs_state_path = c;
    }
    env->ReleaseStringUTFChars(path, c);
    g_bbs_state_result.store(0);
    g_bbs_state_request.store(op);
}

// 0 = pending, 1 = done, -1 = failed.
extern "C" JNIEXPORT jint JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeStateResult(JNIEnv *, jclass) {
    return g_bbs_state_result.load();
}

// Pause holds the game thread at its next vblank wait (state requests still run while paused).
extern "C" JNIEXPORT void JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeSetPaused(JNIEnv *, jclass, jboolean p) {
    g_bbs_paused.store(p == JNI_TRUE);
}

// In-game menu toggle for the texture pack. Returns whether a pack is loaded.
extern "C" JNIEXPORT jboolean JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeSetTextureReplacement(JNIEnv *, jclass,
                                                                                                   jboolean on) {
    texrepl::set_enabled(on == JNI_TRUE);
    return texrepl::pack_loaded() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeSet60Fps(JNIEnv *, jclass, jboolean on) {
    g_bbs_60fps.store(on == JNI_TRUE);
}

extern "C" JNIEXPORT jlong JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeFlipCount(JNIEnv *, jclass) {
    return static_cast<jlong>(g_bbs_flip_count.load(std::memory_order_relaxed));
}

// Boss checkpoints written so far (host/main.cpp): the app shows a notice when it goes up.
extern "C" JNIEXPORT jint JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeBossCheckpoints(JNIEnv *, jclass) {
    return static_cast<jint>(g_bbs_boss_checkpoints.load(std::memory_order_relaxed));
}

// Story slots: who is playing (host/main.cpp): 0 = not known, 1 Ventus, 2 Aqua, 3 Terra.
extern "C" JNIEXPORT jint JNICALL Java_com_psprecomp_bbs_BbsActivity_nativeCharacter(JNIEnv *, jclass) {
    return static_cast<jint>(g_bbs_character.load(std::memory_order_relaxed));
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
    // Graphics capture always armed: idle until `adb shell setprop debug.bbs.capture 1`, then it writes draws and
    // textures to Download/bbs-cap (readable by adb). env.txt may override.
    setenv("PSPRECOMP_GE_CAPTURE", "/storage/emulated/0/Download/bbs-cap", 0);
    // Optional debug switches: files/env.txt, one KEY=VALUE per line (push it with adb).
    const std::string eboot_path = str(eboot);
    // Internal files/env.txt first (writable with `adb shell run-as`); the external one beside the EBOOT is
    // created by adb's shell user (mode 660, group ext_data_rw), which the app cannot read on Android 14+.
    std::string env_file = dir + "/env.txt";
    if (FILE *probe = std::fopen(env_file.c_str(), "r")) std::fclose(probe);
    else env_file = eboot_path.substr(0, eboot_path.find_last_of('/') + 1) + "env.txt";
    if (FILE *f = std::fopen(env_file.c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            const auto eq = l.find('=');
            if (eq == std::string::npos || l[0] == '#') continue;
            setenv(l.substr(0, eq).c_str(), l.substr(eq + 1).c_str(), 1);
            std::cout << "[bbs-android] env " << l << std::endl;
        }
        std::fclose(f);
    }
    // Texture pack (PPSSPP textures.ini format): the user copies it to <external files>/textures/.
    {
        const char *tex_dir = std::getenv("PSPRECOMP_TEX_DIR");
        setenv("PSPRECOMP_TEX_DUMP_FILE", (dir + "/tex-keys.txt").c_str(), 0);
        texrepl::set_directory(tex_dir != nullptr ? std::string(tex_dir)
                                                  : eboot_path.substr(0, eboot_path.find_last_of('/') + 1) + "textures");
    }
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
