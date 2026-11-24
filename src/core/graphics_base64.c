/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "graphics.h"

#include <string.h>

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#define BOXRIG_HAVE_SSSE3_DECODER 1
#include <tmmintrin.h>

/* The high nibble selects one of four alphabet ranges. The low nibble
 * lists the ranges that accept it. A zero intersection marks an invalid
 * byte, including padding. Leave that block to the scalar decoder. */
__attribute__((target("ssse3")))
static size_t decode_blocks(const uint8_t *input, size_t count, uint8_t *output)
{
    const __m128i high_ranges = _mm_setr_epi8(0, 0, 1, 2, 4, 8, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0);
    const __m128i low_ranges = _mm_setr_epi8(10, 14, 14, 14, 14, 14, 14, 14, 14, 14, 12, 5, 4, 4, 4, 5);
    const __m128i offsets = _mm_setr_epi8(0, 0, 19, 4, -65, -65, -71, -71, 0, 0, 0, 0, 0, 0, 0, 0);
    const __m128i order = _mm_setr_epi8(2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1);
    const __m128i nibbles = _mm_set1_epi8(15);
    size_t done = 0;
    while (count - done >= 4) {
        __m128i bytes;
        memcpy(&bytes, input + done * 4, sizeof(bytes));
        __m128i high = _mm_and_si128(_mm_srli_epi16(bytes, 4), nibbles);
        __m128i low = _mm_and_si128(bytes, nibbles);
        __m128i valid = _mm_and_si128(_mm_shuffle_epi8(high_ranges, high),
            _mm_shuffle_epi8(low_ranges, low));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(valid, _mm_setzero_si128()))) break;

        __m128i sextets = _mm_add_epi8(bytes, _mm_shuffle_epi8(offsets, high));
        __m128i slash = _mm_cmpeq_epi8(bytes, _mm_set1_epi8('/'));
        sextets = _mm_sub_epi8(sextets, _mm_and_si128(slash, _mm_set1_epi8(3)));
        /* Combine each quartet into a 24-bit word, then pack its three bytes. */
        __m128i pairs = _mm_maddubs_epi16(sextets, _mm_set1_epi32(0x01400140));
        __m128i words = _mm_madd_epi16(pairs, _mm_set1_epi32(0x00011000));
        __m128i packed = _mm_shuffle_epi8(words, order);
        memcpy(output + done * 3, &packed, 12);
        done += 4;
    }
    return done;
}
#endif

size_t boxrig_graphics_decode_quartets(const uint8_t *input, size_t count, uint8_t *output)
{
    /* Zero means invalid; valid sextets are stored plus one. Neither
     * '=' nor a control byte can be swallowed by this fast path. */
    static const uint8_t values[256] = {
        ['A']=1, ['B']=2, ['C']=3, ['D']=4, ['E']=5, ['F']=6, ['G']=7, ['H']=8,
        ['I']=9, ['J']=10, ['K']=11, ['L']=12, ['M']=13, ['N']=14, ['O']=15, ['P']=16,
        ['Q']=17, ['R']=18, ['S']=19, ['T']=20, ['U']=21, ['V']=22, ['W']=23, ['X']=24,
        ['Y']=25, ['Z']=26, ['a']=27, ['b']=28, ['c']=29, ['d']=30, ['e']=31, ['f']=32,
        ['g']=33, ['h']=34, ['i']=35, ['j']=36, ['k']=37, ['l']=38, ['m']=39, ['n']=40,
        ['o']=41, ['p']=42, ['q']=43, ['r']=44, ['s']=45, ['t']=46, ['u']=47, ['v']=48,
        ['w']=49, ['x']=50, ['y']=51, ['z']=52, ['0']=53, ['1']=54, ['2']=55, ['3']=56,
        ['4']=57, ['5']=58, ['6']=59, ['7']=60, ['8']=61, ['9']=62, ['+']=63, ['/']=64
    };
    size_t done = 0;
#ifdef BOXRIG_HAVE_SSSE3_DECODER
    if (count >= 4 && __builtin_cpu_supports("ssse3"))
        done = decode_blocks(input, count, output);
#endif
    for (; done < count; done++) {
        const uint8_t *p = input + done * 4;
        unsigned int a = values[p[0]], b = values[p[1]], c = values[p[2]], d = values[p[3]];
        if (a == 0 || b == 0 || c == 0 || d == 0) break;
        a--; b--; c--; d--;
        uint8_t *decoded = output + done * 3;
        decoded[0] = (uint8_t)((a << 2) | (b >> 4));
        decoded[1] = (uint8_t)((b << 4) | (c >> 2));
        decoded[2] = (uint8_t)((c << 6) | d);
    }
    return done;
}
