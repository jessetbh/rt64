//
// RT64
//

// [wcw] Jabo Direct3D8 texture pack names. See rt64_wcw_jabo.h.

#include "rt64_wcw_jabo.h"

#include <cctype>
#include <cstring>

namespace RT64 {
    namespace WcwJabo {
        bool Digest::operator==(const Digest &other) const {
            return memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
        }

        size_t DigestHasher::operator()(const Digest &digest) const {
            uint64_t v;
            memcpy(&v, digest.bytes, sizeof(v));
            return size_t(v);
        }

        // MD5, RFC 1321 section 3.4.
        Digest md5(const uint8_t *data, size_t size) {
            static const uint32_t K[64] = {
                0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
                0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
                0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
                0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
                0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
                0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
                0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
                0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
            };
            static const uint8_t R[64] = {
                7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
            };

            uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
            auto block = [&](const uint8_t *p) {
                uint32_t m[16];
                for (int i = 0; i < 16; i++) {
                    m[i] = uint32_t(p[i * 4]) | (uint32_t(p[i * 4 + 1]) << 8) | (uint32_t(p[i * 4 + 2]) << 16) | (uint32_t(p[i * 4 + 3]) << 24);
                }

                uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
                for (int i = 0; i < 64; i++) {
                    uint32_t f;
                    int g;
                    if (i < 16) { f = (b & c) | (~b & d); g = i; }
                    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
                    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
                    else { f = c ^ (b | ~d); g = (7 * i) & 15; }

                    const uint32_t t = d;
                    d = c;
                    c = b;
                    const uint32_t x = a + f + K[i] + m[g];
                    b = b + ((x << R[i]) | (x >> (32 - R[i])));
                    a = t;
                }

                h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            };

            size_t offset = 0;
            for (; (offset + 64) <= size; offset += 64) {
                block(data + offset);
            }

            // Padding: 0x80, zeros, then the bit length (little-endian) in the last 8 bytes.
            uint8_t tail[128] = {};
            const size_t rest = size - offset;
            if (rest > 0) {
                memcpy(tail, data + offset, rest);
            }

            tail[rest] = 0x80;
            const size_t tailSize = (rest < 56) ? 64 : 128;
            const uint64_t bits = uint64_t(size) * 8;
            for (int i = 0; i < 8; i++) {
                tail[tailSize - 8 + i] = uint8_t(bits >> (8 * i));
            }

            for (size_t t = 0; t < tailSize; t += 64) {
                block(tail + t);
            }

            Digest digest;
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    digest.bytes[i * 4 + j] = uint8_t(h[i] >> (8 * j));
                }
            }

            return digest;
        }

        std::string toHex(const Digest &digest) {
            static const char Hex[] = "0123456789abcdef";
            std::string out(32, '0');
            for (int i = 0; i < 16; i++) {
                out[i * 2] = Hex[digest.bytes[i] >> 4];
                out[i * 2 + 1] = Hex[digest.bytes[i] & 0xF];
            }

            return out;
        }

        static int hexValue(char ch) {
            const int c = std::tolower((unsigned char)(ch));
            if ((c >= '0') && (c <= '9')) return c - '0';
            if ((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
            return -1;
        }

        bool parseFileName(const std::string &fileName, Digest &digest) {
            if (fileName.size() != (32 + 4)) {
                return false;
            }

            std::string extension = fileName.substr(32);
            for (char &ch : extension) {
                ch = char(std::tolower((unsigned char)(ch)));
            }

            if ((extension != ".png") && (extension != ".dds")) {
                return false;
            }

            for (int i = 0; i < 16; i++) {
                const int hi = hexValue(fileName[i * 2]);
                const int lo = hexValue(fileName[i * 2 + 1]);
                if ((hi < 0) || (lo < 0)) {
                    return false;
                }

                digest.bytes[i] = uint8_t((hi << 4) | lo);
            }

            return true;
        }

        bool Index::add(const std::string &relativePath) {
            const size_t slash = relativePath.find_last_of("/\\");
            const std::string fileName = (slash == std::string::npos) ? relativePath : relativePath.substr(slash + 1);
            Digest digest;
            if (!parseFileName(fileName, digest)) {
                return false;
            }

            entries.emplace(digest, relativePath);
            return true;
        }

        const std::string *Index::find(const Digest &digest) const {
            auto it = entries.find(digest);
            return (it != entries.end()) ? &it->second : nullptr;
        }

        uint16_t fromRGBA16(uint16_t rgba5551) {
            return uint16_t(((rgba5551 & 1) << 15) | (rgba5551 >> 1));
        }

        uint16_t fromIntensity(uint32_t intensity8, bool opaque) {
            const uint16_t i5 = uint16_t((intensity8 & 0xFF) >> 3);
            return uint16_t((opaque ? 0x8000 : 0) | (i5 << 10) | (i5 << 5) | i5);
        }

        bool decodeA1R5G5B5(uint8_t fmt, uint8_t siz, const uint8_t *texels, uint32_t width, uint32_t height, uint32_t rowStride,
            const uint8_t *palette, std::vector<uint8_t> &out)
        {
            out.clear();
            if ((width == 0) || (height == 0) || (siz > Siz16b) || (fmt == FmtYUV) || ((fmt == FmtCI) && (palette == nullptr)) ||
                ((fmt == FmtCI) && (siz == Siz16b)))
            {
                return false;
            }

            auto paletteColor = [&](uint32_t index) {
                return fromRGBA16(uint16_t((palette[index * 2] << 8) | palette[index * 2 + 1]));
            };

            out.reserve(size_t(width) * height * 2);
            for (uint32_t y = 0; y < height; y++) {
                const uint8_t *row = texels + size_t(y) * rowStride;
                for (uint32_t x = 0; x < width; x++) {
                    uint16_t color = 0;
                    switch (siz) {
                    case Siz4b: {
                        const uint8_t b = row[x >> 1];
                        const uint32_t v = (x & 1) ? (b & 0xF) : (b >> 4);
                        if (fmt == FmtCI) color = paletteColor(v);
                        else if (fmt == FmtIA) color = fromIntensity(((v >> 1) * 255) / 7, (v & 1) != 0);
                        else color = fromIntensity(v * 17, true);
                        break;
                    }
                    case Siz8b: {
                        const uint32_t v = row[x];
                        if (fmt == FmtCI) color = paletteColor(v);
                        else if (fmt == FmtIA) color = fromIntensity((v >> 4) * 17, (v & 0xF) >= 8);
                        else color = fromIntensity(v, true);
                        break;
                    }
                    default: {
                        const uint16_t c = uint16_t((row[x * 2] << 8) | row[x * 2 + 1]);
                        color = (fmt == FmtIA) ? fromIntensity(c >> 8, (c & 0xFF) >= 0x80) : fromRGBA16(c);
                        break;
                    }
                    }

                    out.push_back(uint8_t(color));
                    out.push_back(uint8_t(color >> 8));
                }
            }

            return true;
        }

        bool textureDigest(uint8_t fmt, uint8_t siz, const uint8_t *texels, uint32_t width, uint32_t height, uint32_t rowStride,
            const uint8_t *palette, Digest &digest)
        {
            std::vector<uint8_t> pixels;
            if (!decodeA1R5G5B5(fmt, siz, texels, width, height, rowStride, palette, pixels)) {
                return false;
            }

            digest = md5(pixels.data(), pixels.size());
            return true;
        }
    };
};
