#include "gpu/vdp1/vdp1_op_dump.hpp"

#include <cstring>
#include <fstream>
#include <type_traits>

namespace brimir::gpu::vdp1 {

namespace {

constexpr char kMagic[8] = {'B', 'R', 'V', '1', 'O', 'P', 'S', '1'};
constexpr uint32_t kMaxElements = 1u << 28;

class Writer {
public:
    explicit Writer(std::ofstream &out)
        : m_out(out) {}

    template <typename T>
    void Value(const T &v) {
        static_assert(std::is_trivially_copyable_v<T>);
        m_out.write(reinterpret_cast<const char *>(&v), sizeof(T));
    }
    template <typename T>
    void Vector(const std::vector<T> &v) {
        static_assert(std::is_trivially_copyable_v<T>);
        Value(static_cast<uint32_t>(v.size()));
        if (!v.empty()) {
            m_out.write(reinterpret_cast<const char *>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(T)));
        }
    }

private:
    std::ofstream &m_out;
};

class Reader {
public:
    explicit Reader(std::ifstream &in)
        : m_in(in) {}

    template <typename T>
    bool Value(T &v) {
        static_assert(std::is_trivially_copyable_v<T>);
        m_in.read(reinterpret_cast<char *>(&v), sizeof(T));
        return static_cast<bool>(m_in);
    }
    template <typename T>
    bool Vector(std::vector<T> &v) {
        static_assert(std::is_trivially_copyable_v<T>);
        uint32_t n = 0;
        if (!Value(n) || n > kMaxElements) {
            return false;
        }
        v.resize(n);
        if (n != 0) {
            m_in.read(reinterpret_cast<char *>(v.data()), static_cast<std::streamsize>(size_t{n} * sizeof(T)));
        }
        return static_cast<bool>(m_in);
    }

private:
    std::ifstream &m_in;
};

template <typename Io, typename List>
bool ListFields(Io &io, List &l) {
    return io.Vector(l.ops) && io.Vector(l.segments) && io.Vector(l.batches) && io.Vector(l.commands) &&
           io.Vector(l.spans) && io.Vector(l.vramCopies) && io.Vector(l.arena) && io.Vector(l.erases) &&
           io.Vector(l.fbWrites) && io.Vector(l.fbWriteRuns);
}

void Fail(std::string *error, const std::string &text) {
    if (error != nullptr) {
        *error = text;
    }
}

} // namespace

bool WriteVdp1OpDump(const std::string &path, const Vdp1OpDump &d, std::string *error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        Fail(error, "cannot create " + path);
        return false;
    }
    out.write(kMagic, sizeof(kMagic));
    Writer w(out);
    w.Value(d.frame);
    w.Vector(d.fbramStart);
    w.Vector(d.vramStart);
    w.Value(static_cast<uint32_t>(d.lists.size()));
    for (const Vdp1OpList &l : d.lists) {
        w.Vector(l.ops);
        w.Vector(l.segments);
        w.Vector(l.batches);
        w.Vector(l.commands);
        w.Vector(l.spans);
        w.Vector(l.vramCopies);
        w.Vector(l.arena);
        w.Vector(l.erases);
        w.Vector(l.fbWrites);
        w.Vector(l.fbWriteRuns);
    }
    w.Vector(d.fbramEnd);
    w.Value(d.drawFB);
    w.Value(d.displayFB);
    w.Value(d.hres);
    w.Value(d.vres);
    w.Value(d.exclusiveMonitor);
    w.Value(d.deinterlace);
    w.Value(d.transparentMeshes);
    w.Value(d.tvmr);
    w.Value(d.fbcr);
    w.Value(d.vdp2Regs);
    w.Value(d.palette);
    out.flush();
    if (!out) {
        Fail(error, "write failed: " + path);
        return false;
    }
    return true;
}

bool ReadVdp1OpDump(const std::string &path, Vdp1OpDump &d, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        Fail(error, "cannot open " + path);
        return false;
    }
    char magic[sizeof(kMagic)] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        Fail(error, "not a VDP1 op dump: " + path);
        return false;
    }
    Reader r(in);
    uint32_t lists = 0;
    bool ok = r.Value(d.frame) && r.Vector(d.fbramStart) && r.Vector(d.vramStart) && r.Value(lists) && lists < 4096;
    if (ok) {
        d.lists.assign(lists, {});
        for (Vdp1OpList &l : d.lists) {
            ok = ok && ListFields(r, l);
        }
    }
    ok = ok && r.Vector(d.fbramEnd) && r.Value(d.drawFB) && r.Value(d.displayFB) && r.Value(d.hres) &&
         r.Value(d.vres) && r.Value(d.exclusiveMonitor) && r.Value(d.deinterlace) && r.Value(d.transparentMeshes) &&
         r.Value(d.tvmr) && r.Value(d.fbcr) && r.Value(d.vdp2Regs) && r.Value(d.palette);
    if (!ok || d.fbramStart.size() != kFbramBufferSize || d.fbramEnd.size() != kFbramBufferSize) {
        Fail(error, "truncated or malformed VDP1 op dump: " + path);
        return false;
    }
    return true;
}

} // namespace brimir::gpu::vdp1
