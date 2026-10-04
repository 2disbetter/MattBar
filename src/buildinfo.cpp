// Build identity of the running binary. See buildinfo.hpp.
#include "buildinfo.hpp"
#include "config.hpp"

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace {

std::string hex(const unsigned char* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

// dl_iterate_phdr visits the main program first; walk its PT_NOTE segments
// for the GNU build-id note, then stop.
int scan_main_program(dl_phdr_info* info, size_t, void* data) {
    auto* out = static_cast<std::string*>(data);
    for (int i = 0; i < info->dlpi_phnum && out->empty(); ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const size_t align = ph.p_align == 8 ? 8 : 4;
        auto up = [align](size_t v) { return (v + align - 1) & ~(align - 1); };
        const char* p   = reinterpret_cast<const char*>(info->dlpi_addr + ph.p_vaddr);
        const char* end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            ElfW(Nhdr) nh;
            memcpy(&nh, p, sizeof nh);
            const char* name = p + sizeof nh;
            const char* desc = name + up(nh.n_namesz);
            const char* next = desc + up(nh.n_descsz);
            if (next > end || next <= p) break;
            if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 &&
                memcmp(name, "GNU", 4) == 0 && nh.n_descsz > 0) {
                *out = hex(reinterpret_cast<const unsigned char*>(desc),
                           nh.n_descsz);
                break;
            }
            p = next;
        }
    }
    return 1; // main program only
}

// Linked without a build-id (a toolchain that ignores the CMake flag):
// hash the running image's file instead. 64-bit FNV-1a, read once.
std::string hash_file(int fd) {
    uint64_t h = 1469598103934665603ULL;
    unsigned char buf[65536];
    ssize_t n;
    lseek(fd, 0, SEEK_SET);
    while ((n = read(fd, buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < n; ++i) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(h >> (56 - 8 * i));
    return hex(b, 8);
}

std::string fmt_time(time_t t) {
    tm lt{};
    localtime_r(&t, &lt);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &lt);
    return b;
}

// Path the kernel reports for our executable, minus the " (deleted)" it
// appends once that file has been unlinked or replaced.
std::string exe_path() {
    char b[4096];
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    if (n <= 0) return {};
    std::string p(b, static_cast<size_t>(n));
    static const std::string del = " (deleted)";
    if (p.size() > del.size() &&
        p.compare(p.size() - del.size(), del.size(), del) == 0)
        p.resize(p.size() - del.size());
    return p;
}

struct Cache {
    BuildInfo bi;
    dev_t     dev = 0;
    ino_t     ino = 0;
    bool      have_inode = false;
};

const Cache& cache() {
    static const Cache c = [] {
        Cache r;
        r.bi.version = MATTBAR_VERSION;
        r.bi.path    = exe_path();
        dl_iterate_phdr(scan_main_program, &r.bi.id);
        // /proc/self/exe opens the image we run even after the file at
        // its path was replaced: its mtime is when THIS build was linked
        // (or installed), and its inode tells a replacement apart.
        int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat st {};
            if (fstat(fd, &st) == 0) {
                r.bi.linked   = fmt_time(st.st_mtime);
                r.dev         = st.st_dev;
                r.ino         = st.st_ino;
                r.have_inode  = true;
            }
            if (r.bi.id.empty()) r.bi.id = hash_file(fd);
            close(fd);
        }
        if (r.bi.id.empty()) r.bi.id = "unknown";
        r.bi.short_id = r.bi.id.substr(0, 7);
        return r;
    }();
    return c;
}

} // namespace

const BuildInfo& build_info() { return cache().bi; }

std::string build_line() {
    const BuildInfo& b = build_info();
    std::string s = b.version + " \u00b7 build " + b.short_id;
    if (!b.linked.empty()) s += " \u00b7 " + b.linked;
    return s;
}

ExeState exe_state(std::string* disk_linked) {
    const Cache& c = cache();
    if (c.bi.path.empty() || !c.have_inode) return ExeState::Same;
    struct stat st {};
    if (stat(c.bi.path.c_str(), &st) != 0) return ExeState::Gone;
    if (st.st_dev == c.dev && st.st_ino == c.ino) return ExeState::Same;
    if (disk_linked) *disk_linked = fmt_time(st.st_mtime);
    return ExeState::Replaced;
}
