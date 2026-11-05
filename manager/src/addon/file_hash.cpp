#include "file_hash.h"
#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace eam {

namespace {

// Closes a CNG algorithm provider and hash object however the function that opened them ends.
struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> state;

    bool Open() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
        DWORD stateSize = 0, got = 0;
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&stateSize, sizeof stateSize, &got, 0) != 0) return false;
        state.resize(stateSize);
        return BCryptCreateHash(alg, &hash, state.data(), stateSize, nullptr, 0, 0) == 0;
    }
    ~Sha256() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
};

} // namespace

std::string FileSHA256(const std::wstring& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) return {};

    Sha256 sha;
    if (!sha.Open()) return {};

    std::vector<char> chunk(64 * 1024);
    while (in) {
        in.read(chunk.data(), (std::streamsize)chunk.size());
        const std::streamsize got = in.gcount();
        if (got > 0 && BCryptHashData(sha.hash, (PUCHAR)chunk.data(), (ULONG)got, 0) != 0) return {};
    }

    UCHAR digest[32];
    if (BCryptFinishHash(sha.hash, digest, sizeof digest, 0) != 0) return {};

    static const char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (UCHAR byte : digest) {
        hex.push_back(kDigits[byte >> 4]);
        hex.push_back(kDigits[byte & 0x0F]);
    }
    return hex;
}

} // namespace eam
