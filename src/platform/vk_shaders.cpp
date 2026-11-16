// HLSL to SPIR-V at runtime, in place of D3DCompile: glslangValidator runs as a child process and the .spv it writes
// stays in a cache keyed by the source, so a shader compiles once and a hit never leaves the process
#include "platform/vk_shaders.h"

#include <crt_externs.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"

namespace gpu::vk {

namespace {

std::atomic<uint32_t> g_compiled{0};
std::atomic<uint32_t> g_cached{0};

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t HashBytes(const void* p, size_t n, uint64_t h) {
    auto bytes = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ bytes[i]) * kFnvPrime;
    return h;
}

uint64_t HashText(const std::string& s, uint64_t h = kFnvOffset) { return HashBytes(s.data(), s.size(), h); }

// the cache file name and the validator's -S
struct StageInfo {
    const char* name;
    const char* glslang;
};

constexpr StageInfo kStages[] = {{"vs", "vert"}, {"ps", "frag"}, {"gs", "geom"}, {"cs", "comp"}};

static_assert(sizeof(kStages) / sizeof(kStages[0]) == size_t(Stage::Compute) + 1, "a Stage per table entry");

// where the host's descriptor set puts them: two cbuffers at 0 and 1, the images from kBindingTextures, the samplers
// from kBindingSamplers (vk_context.h). HLSL's register spaces shift onto that
struct BindingShift {
    long texture = 2;
    long sampler = 6;
};

long EnvNumber(const char* name, long fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    long n = std::strtol(v, &end, 10);
    return end != v && n >= 0 ? n : fallback;
}

const BindingShift& Shifts() {
    static const BindingShift shifts{EnvNumber("BLACK_HLSL_TEXTURE_BASE", 2),
                                     EnvNumber("BLACK_HLSL_SAMPLER_BASE", 6)};
    return shifts;
}

bool Executable(const std::string& path) { return !path.empty() && access(path.c_str(), X_OK) == 0; }

std::string Join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

// the validator: BLACK_GLSLANG, a copy beside the exe, then PATH, then the install places
const std::string& GlslangPath() {
    static const std::string path = [] {
        if (const char* env = std::getenv("BLACK_GLSLANG"); env && Executable(env)) return std::string(env);
        if (const char* base = std::getenv("BLACK_BASE_PATH"); base && Executable(Join(base, "glslangValidator")))
            return Join(base, "glslangValidator");
        if (const char* env = std::getenv("PATH"); env) {
            std::string rest = env;
            while (!rest.empty()) {
                size_t at = rest.find(':');
                std::string found = Join(rest.substr(0, at), "glslangValidator");
                if (Executable(found)) return found;
                if (at == std::string::npos) break;
                rest.erase(0, at + 1);
            }
        }
        for (const char* dir : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"}) {
            std::string found = Join(dir, "glslangValidator");
            if (Executable(found)) return found;
        }
        return std::string();
    }();
    return path;
}

// the compiled shaders' home: BLACK_SHADER_CACHE, else the user's cache directory
const std::string& CacheDir() {
    static const std::string dir = [] {
        std::string d;
        if (const char* env = std::getenv("BLACK_SHADER_CACHE"); env && *env) {
            d = env;
        } else if (const char* home = std::getenv("HOME"); home && *home) {
            std::string base = std::string(home) + "/Library/Caches";
            d = base + "/black/shaders";
            mkdir(base.c_str(), 0755);
            mkdir((base + "/black").c_str(), 0755);
        }
        // a read-only home must not fail every shader: fall back to the temporary directory
        auto usable = [](const std::string& p) {
            mkdir(p.c_str(), 0755);
            return access(p.c_str(), W_OK) == 0;
        };
        if (!d.empty() && usable(d)) return d;
        const char* tmp = std::getenv("TMPDIR");
        std::string fallback = Join(tmp && *tmp ? tmp : "/tmp", "black-shaders");
        usable(fallback);
        return fallback;
    }();
    return dir;
}

// identifies the validator, so an upgrade invalidates the modules it did not write
uint64_t ValidatorStamp() {
    struct stat st{};
    if (stat(GlslangPath().c_str(), &st) != 0) return 0;
    uint64_t h = HashBytes(&st.st_mtime, sizeof(st.st_mtime), kFnvOffset);
    return HashBytes(&st.st_size, sizeof(st.st_size), h);
}

// the child's stdout and stderr come back together: glslang puts its errors there
bool RunProcess(const std::vector<std::string>& args, std::string& output, int& spawn_error) {
    spawn_error = 0;
    int fds[2];
    if (pipe(fds) != 0) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    // nothing of ours reaches the child: the window and audio descriptors stay in this process
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    pid_t pid = 0;
    int spawned = posix_spawn(&pid, argv[0], &actions, &attr, argv.data(), *_NSGetEnviron());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    output.clear();
    if (spawned != 0) {
        spawn_error = spawned;
        close(fds[0]);
        return false;
    }
    // a wedged validator must not hold the render thread for ever
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    bool timed_out = false;
    char buf[4096];
    for (;;) {
        pollfd pfd{fds[0], POLLIN, 0};
        int ready = poll(&pfd, 1, 1000);
        if (ready == 0) {
            if (std::chrono::steady_clock::now() < deadline) continue;
            timed_out = true;
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        output.append(buf, size_t(n));
    }
    close(fds[0]);
    if (timed_out) kill(pid, SIGKILL);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    if (timed_out) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[4096];
    for (;;) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        out.insert(out.end(), buf, buf + n);
        if (n < sizeof(buf)) break;
    }
    bool ok = std::ferror(f) == 0 && !out.empty();
    std::fclose(f);
    return ok;
}

bool WriteFile(const std::string& path, const void* data, size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, size, f) == size;
    return std::fclose(f) == 0 && ok;
}

std::vector<uint32_t> ToWords(const std::vector<uint8_t>& bytes) {
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), words.size() * sizeof(uint32_t));
    return words;
}

// glslang only warns when the entry point is missing and writes a module without it, so the words are checked.
// OpEntryPoint holds the execution model, the function's id, then the name as a literal string
bool NamesEntryPoint(const std::vector<uint32_t>& words, const char* entry) {
    constexpr uint32_t kOpEntryPoint = 15;
    size_t n = std::strlen(entry);
    for (size_t i = 5; i < words.size();) {
        uint32_t count = words[i] >> 16;
        if (count == 0) return false;
        if ((words[i] & 0xffff) == kOpEntryPoint && count > 3 && i + 3 < words.size()) {
            auto name = reinterpret_cast<const char*>(&words[i + 3]);
            size_t room = std::min<size_t>(count - 3, words.size() - i - 3) * sizeof(uint32_t);
            if (room > n && std::strncmp(name, entry, n) == 0 && name[n] == '\0') return true;
        }
        i += count;
    }
    return false;
}

}  // namespace

CacheStats Stats() { return {g_compiled.load(), g_cached.load()}; }

const char* StageName(Stage stage) {
    auto i = unsigned(stage);
    return i < sizeof(kStages) / sizeof(kStages[0]) ? kStages[i].name : "vs";
}

bool CompilerAvailable(std::string& error) {
    if (!GlslangPath().empty()) return true;
    error = "no glslangValidator: set BLACK_GLSLANG, ship one beside the exe, or put one on PATH";
    return false;
}

std::vector<uint32_t> CompileHlsl(const std::string& src, const char* entry, Stage stage, std::string& error) {
    error.clear();
    std::vector<uint32_t> words;
    unsigned si = unsigned(stage);
    if (si >= sizeof(kStages) / sizeof(kStages[0])) {
        error = "not a shader stage";
        return words;
    }
    const char* stage_name = kStages[si].name;
    if (!entry || !*entry) entry = "main";

    // the source, the entry, the stage and the binding shift decide the module: the file is named for all four
    const BindingShift& shifts = Shifts();
    uint64_t key = HashText(src, HashText(entry, HashText(stage_name)));
    key = HashBytes(&shifts.texture, sizeof(shifts.texture), key);
    key = HashBytes(&shifts.sampler, sizeof(shifts.sampler), key);
    // a new validator, or a change to the arguments below, must not keep serving an older module
    constexpr uint32_t kCacheVersion = 2;
    uint64_t stamp = ValidatorStamp();
    key = HashBytes(&kCacheVersion, sizeof(kCacheVersion), key);
    key = HashBytes(&stamp, sizeof(stamp), key);
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(key));

    const std::string& dir = CacheDir();
    LOG_FIRST(1, "vk_shaders: cache %s", dir.c_str());
    if (dir.empty()) {
        error = "no shader cache directory: set BLACK_SHADER_CACHE";
        return words;
    }
    const std::string spv = Join(dir, std::string(name) + ".spv");
    // glslang writes its output in place: with one shared name two processes read each other's half written module
    static std::atomic<uint32_t> temp_id{0};
    const std::string tmp = Join(dir, std::string(name) + "." + std::to_string(getpid()) + "." +
                                           std::to_string(temp_id.fetch_add(1)) + ".tmp");
    std::vector<uint8_t> bytes;
    if (ReadFile(spv, bytes) && bytes.size() % sizeof(uint32_t) == 0) {
        words = ToWords(bytes);
        if (NamesEntryPoint(words, entry)) {
            g_cached++;
            return words;
        }
        unlink(spv.c_str());  // a cache entry from an earlier run that has no such entry
        words.clear();
    }

    const std::string& exe = GlslangPath();
    if (exe.empty()) {
        error = "no glslangValidator: set BLACK_GLSLANG, ship one beside the exe, or put one on PATH";
        return words;
    }

    // the validator takes a file: the pid in the name keeps two runs from overwriting each other's source
    const std::string hlsl = Join(dir, std::string(name) + "." + std::to_string(getpid()) + ".hlsl");
    if (!WriteFile(hlsl, src.data(), src.size())) {
        error = "cannot write " + hlsl;
        unlink(hlsl.c_str());  // a half written source must not be left for a later run
        return words;
    }

    // glslang's HLSL frontend: vulkan 1.1, the entry point, and the register spaces shifted onto the host's layout
    std::vector<std::string> args = {exe,
                                     "-D",
                                     "-V",
                                     "--target-env",
                                     "vulkan1.1",
                                     "-S",
                                     kStages[si].glslang,
                                     "-e",
                                     entry,
                                     "-o",
                                     tmp,
                                     "--shift-texture-binding",
                                     stage_name,
                                     std::to_string(shifts.texture),
                                     "--shift-sampler-binding",
                                     stage_name,
                                     std::to_string(shifts.sampler),
                                     hlsl};
    std::string output;
    int spawn_error = 0;
    bool ok = RunProcess(args, output, spawn_error);
    unlink(hlsl.c_str());
    if (!ok) {
        error = !output.empty()              ? output
                : spawn_error                ? "glslangValidator failed: " + std::string(std::strerror(spawn_error))
                                             : "glslangValidator failed";
        // the temp file's name means nothing to a reader: the caller knows which entry it asked for
        for (size_t at = error.find(hlsl); at != std::string::npos; at = error.find(hlsl, at))
            error.replace(at, hlsl.size(), "<hlsl>");
        unlink(tmp.c_str());  // only this run's output: another run's cached module stays
        return words;
    }
    if (!ReadFile(tmp, bytes) || bytes.size() % sizeof(uint32_t) != 0) {
        error = "glslangValidator wrote no usable " + spv;
        unlink(tmp.c_str());
        return words;
    }
    words = ToWords(bytes);
    if (!NamesEntryPoint(words, entry)) {
        // glslang warned rather than failed: the module holds no such entry point
        error = "no entry point '" + std::string(entry) + "' for " + stage_name;
        unlink(tmp.c_str());
        words.clear();
        return words;
    }
    // the rename is what publishes the entry: a reader never sees a module that is still being written
    if (rename(tmp.c_str(), spv.c_str()) != 0) unlink(tmp.c_str());
    g_compiled++;
    LOGI("vk_shaders: %s %s compiled, %zu bytes", stage_name, entry, bytes.size());
    return words;
}

VkShaderModule CreateShaderModule(VkDevice dev, const std::string& src, const char* entry, Stage stage,
                                  std::string& error) {
    std::vector<uint32_t> words = CompileHlsl(src, entry, stage, error);
    if (words.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * sizeof(uint32_t);
    ci.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &module) != VK_SUCCESS) {
        error = "the driver refused the module";
        return VK_NULL_HANDLE;
    }
    return module;
}

}  // namespace gpu::vk
