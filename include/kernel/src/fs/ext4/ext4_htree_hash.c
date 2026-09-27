#include "ext4_htree.h"
#include "lib/string.h"
#include <stdint.h>

#define MD4_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define MD4_G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define MD4_H(x, y, z) ((x) ^ (y) ^ (z))

#define ROL32(value, shift) \
    (((value) << (shift)) | ((value) >> (32 - (shift))))

#define MD4_ROUND(function, a, b, c, d, value, shift) \
    do {                                                   \
        (a) += function((b), (c), (d)) + (value);          \
        (a) = ROL32((a), (shift));                         \
    } while (0)

#define MD4_K2 013240474631u
#define MD4_K3 015666365641u

static void build_hash_buffer(
    const char *name,
    int name_length,
    uint32_t *output,
    int output_words,
    int unsigned_chars)
{
    uint32_t padding;
    uint32_t value;
    int words_left;

    padding = (uint32_t)name_length |
              ((uint32_t)name_length << 8);

    padding |= padding << 16;

    value = padding;
    words_left = output_words;

    if (name_length > output_words * 4)
        name_length = output_words * 4;

    for (int i = 0; i < name_length; i++) {
        int character;

        if (unsigned_chars)
            character = (int)(uint8_t)name[i];
        else
            character = (int)(int8_t)name[i];

        value = (uint32_t)character + (value << 8);

        if ((i & 3) == 3) {
            *output++ = value;
            value = padding;
            words_left--;
        }
    }

    if (--words_left >= 0)
        *output++ = value;

    while (--words_left >= 0)
        *output++ = padding;
}

static void half_md4_transform(
    uint32_t state[4],
    const uint32_t input[8])
{
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    /* Round 1 */
    MD4_ROUND(MD4_F, a, b, c, d, input[0], 3);
    MD4_ROUND(MD4_F, d, a, b, c, input[1], 7);
    MD4_ROUND(MD4_F, c, d, a, b, input[2], 11);
    MD4_ROUND(MD4_F, b, c, d, a, input[3], 19);

    MD4_ROUND(MD4_F, a, b, c, d, input[4], 3);
    MD4_ROUND(MD4_F, d, a, b, c, input[5], 7);
    MD4_ROUND(MD4_F, c, d, a, b, input[6], 11);
    MD4_ROUND(MD4_F, b, c, d, a, input[7], 19);

    /* Round 2 */
    MD4_ROUND(MD4_G, a, b, c, d, input[1] + MD4_K2, 3);
    MD4_ROUND(MD4_G, d, a, b, c, input[3] + MD4_K2, 5);
    MD4_ROUND(MD4_G, c, d, a, b, input[5] + MD4_K2, 9);
    MD4_ROUND(MD4_G, b, c, d, a, input[7] + MD4_K2, 13);

    MD4_ROUND(MD4_G, a, b, c, d, input[0] + MD4_K2, 3);
    MD4_ROUND(MD4_G, d, a, b, c, input[2] + MD4_K2, 5);
    MD4_ROUND(MD4_G, c, d, a, b, input[4] + MD4_K2, 9);
    MD4_ROUND(MD4_G, b, c, d, a, input[6] + MD4_K2, 13);

    /* Round 3 */
    MD4_ROUND(MD4_H, a, b, c, d, input[3] + MD4_K3, 3);
    MD4_ROUND(MD4_H, d, a, b, c, input[7] + MD4_K3, 9);
    MD4_ROUND(MD4_H, c, d, a, b, input[2] + MD4_K3, 11);
    MD4_ROUND(MD4_H, b, c, d, a, input[6] + MD4_K3, 15);

    MD4_ROUND(MD4_H, a, b, c, d, input[1] + MD4_K3, 3);
    MD4_ROUND(MD4_H, d, a, b, c, input[5] + MD4_K3, 9);
    MD4_ROUND(MD4_H, c, d, a, b, input[0] + MD4_K3, 11);
    MD4_ROUND(MD4_H, b, c, d, a, input[4] + MD4_K3, 15);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

int ext4_htree_hash(
    ext2_mount_t *mount,
    uint8_t version,
    const char *name,
    int name_length,
    uint32_t *result_hash)
{
    uint32_t state[4] = {
        0x67452301,
        0xefcdab89,
        0x98badcfe,
        0x10325476
    };

    const char *current;
    int bytes_left;

    if (version != 1 && version != 4)
        return -1;

    if (mount->sb.s_hash_seed[0] ||
        mount->sb.s_hash_seed[1] ||
        mount->sb.s_hash_seed[2] ||
        mount->sb.s_hash_seed[3]) {
        memcpy(
            state,
            mount->sb.s_hash_seed,
            sizeof(state)
        );
    }

    current = name;
    bytes_left = name_length;

    while (bytes_left > 0) {
        uint32_t hash_input[8];

        build_hash_buffer(
            current,
            bytes_left,
            hash_input,
            8,
            version == 4
        );

        half_md4_transform(state, hash_input);

        current += 32;
        bytes_left -= 32;
    }

    *result_hash = state[1] & 0xfffffffeu;

    if (*result_hash == 0xfffffffeu)
        *result_hash = 0xfffffffcu;

    return 0;
}
