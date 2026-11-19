// bbs_decrypt: decrypts a PSP executable (EBOOT.BIN, "~PSP") on the device, for the app's first-run setup (C2).
// Built as its own executable (packaged as lib/arm64-v8a/libbbsdecrypt.so so Android extracts it) and run by the
// app as a separate process: `libbbsdecrypt.so <in> <out>`. It links pspdecrypt's PrxDecrypter and libkirk
// (third_party/pspdecrypt, GPL-3.0), so this executable is distributed under GPL-3.0; its source is this file plus
// that submodule. The app itself (MIT) only runs it and reads the file it writes.
// Exit codes: 0 ok, 1 usage, 2 read error, 3 not an encrypted PSP executable, 4 decryption failed, 5 write error,
// 6 decompression failed.
#include "PrxDecrypter.h"
extern "C" {
#include "kl4e.h"
#include "libkirk/kirk_engine.h"
#include "libkirk/amctrl.h"
}

#include <cstdint>
#include <string>

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
bool read_file(const char *path, std::vector<u8> &out) {
    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? static_cast<std::size_t>(size) : 0u);
    const bool ok = size > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

bool gunzip(const std::vector<u8> &in, std::vector<u8> &out, std::size_t capacity) {
    out.resize(capacity);
    z_stream z{};
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return false;
    z.next_in = const_cast<Bytef *>(in.data());
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    const int r = inflate(&z, Z_FINISH);
    out.resize(z.total_out);
    inflateEnd(&z);
    return r == Z_STREAM_END;
}
// PGD (amctrl DRM) game data, e.g. BBS1-3.DAT: the game passes the 16-byte key to sceIoIoctl 0x04100001 and the
// host runs `libbbsdecrypt.so pgd <in> <out> <key hex>` the first time (disc_io.cpp). Same steps as
// psp-native-runtime/tools/pspdec/pgddec.cpp. Writes <out>.part, renamed to <out> when complete.
int decrypt_pgd(const char *in_path, const char *out_path, const char *key_hex) {
    u8 key[16];
    if (std::strlen(key_hex) != 32) { std::fprintf(stderr, "key must be 32 hex characters\n"); return 1; }
    for (int i = 0; i < 16; ++i) {
        unsigned v = 0;
        if (std::sscanf(key_hex + i * 2, "%2x", &v) != 1) { std::fprintf(stderr, "bad key\n"); return 1; }
        key[i] = static_cast<u8>(v);
    }
    std::FILE *in = std::fopen(in_path, "rb");
    if (in == nullptr) { std::fprintf(stderr, "cannot read %s\n", in_path); return 2; }
    u8 header[0x90];
    if (std::fread(header, 1, sizeof header, in) != sizeof header || std::memcmp(header, "\0PGD", 4) != 0) {
        std::fclose(in);
        std::fprintf(stderr, "not a PGD file\n");
        return 3;
    }
    kirk_init();
    PGD_DESC *pgd = nullptr;
    u8 work[sizeof header];
    for (int flag : {2, 1}) {  // the DNAS key the header MACs are checked with: 2 for ioctl 0x04100001 data
        std::memcpy(work, header, sizeof header);
        pgd = pgd_open(work, flag, key);
        if (pgd != nullptr) break;
    }
    if (pgd == nullptr) { std::fclose(in); std::fprintf(stderr, "PGD header check failed (wrong key?)\n"); return 4; }
    const std::string part = std::string(out_path) + ".part";
    std::FILE *out = std::fopen(part.c_str(), "wb");
    if (out == nullptr) { pgd_close(pgd); std::fclose(in); std::fprintf(stderr, "cannot write %s\n", part.c_str()); return 5; }
    std::uint64_t written = 0;
    bool ok = true;
    for (u32 block = 0; block < pgd->block_nr && ok; ++block) {
        std::memset(pgd->block_buf, 0, pgd->block_size);
        std::fseek(in, static_cast<long>(pgd->data_offset + static_cast<std::uint64_t>(block) * pgd->block_size), SEEK_SET);
        (void)std::fread(pgd->block_buf, 1, pgd->block_size, in);
        pgd_decrypt_block(pgd, static_cast<int>(block));
        const std::uint64_t left = pgd->data_size - written;
        const std::uint64_t take = left < pgd->block_size ? left : pgd->block_size;
        ok = std::fwrite(pgd->block_buf, 1, take, out) == take;
        written += take;
    }
    pgd_close(pgd);
    std::fclose(in);
    ok = std::fclose(out) == 0 && ok;
    if (!ok || std::rename(part.c_str(), out_path) != 0) {
        std::remove(part.c_str());
        std::fprintf(stderr, "write failed\n");
        return 5;
    }
    std::printf("ok %llu bytes\n", static_cast<unsigned long long>(written));
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 5 && std::strcmp(argv[1], "pgd") == 0) return decrypt_pgd(argv[2], argv[3], argv[4]);
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <EBOOT.BIN> <out.elf>\n       %s pgd <in.DAT> <out> <key hex>\n", argv[0], argv[0]);
        return 1;
    }
    std::vector<u8> in;
    if (!read_file(argv[1], in)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    if (in.size() < 0x150 || std::memcmp(in.data(), "~PSP", 4) != 0) {
        std::fprintf(stderr, "not an encrypted PSP executable\n");
        return 3;
    }
    std::vector<u8> out(in.size());
    const int size = pspDecryptPRX(in.data(), out.data(), static_cast<u32>(in.size()), nullptr, false);
    if (size <= 0) { std::fprintf(stderr, "decryption failed (%d)\n", size); return 4; }
    out.resize(static_cast<std::size_t>(size));
    // A compressed executable: the header's elf_size (0x28) is the size after decompression.
    u32 elf_size = 0;
    std::memcpy(&elf_size, in.data() + 0x28, 4);
    if (out.size() >= 4 && out[0] == 0x1F && out[1] == 0x8B) {
        std::vector<u8> plain;
        if (!gunzip(out, plain, elf_size)) { std::fprintf(stderr, "gzip failed\n"); return 6; }
        out.swap(plain);
    } else if (out.size() >= 4 && (std::memcmp(out.data(), "KL4E", 4) == 0 || std::memcmp(out.data(), "KL3E", 4) == 0)) {
        std::vector<u8> plain(elf_size);
        const int n = decompress_kle(plain.data(), static_cast<int>(plain.size()), out.data() + 4, nullptr,
                                     out[2] == '4');
        if (n <= 0) { std::fprintf(stderr, "kl4e failed (%d)\n", n); return 6; }
        plain.resize(static_cast<std::size_t>(n));
        out.swap(plain);
    }
    std::FILE *f = std::fopen(argv[2], "wb");
    if (f == nullptr || std::fwrite(out.data(), 1, out.size(), f) != out.size()) {
        if (f != nullptr) std::fclose(f);
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 5;
    }
    std::fclose(f);
    std::printf("ok %zu bytes\n", out.size());
    return 0;
}
