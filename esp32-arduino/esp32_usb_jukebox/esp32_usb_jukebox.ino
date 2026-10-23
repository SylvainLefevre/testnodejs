/*
 * ESP32-S3 + MAX98357A + clé USB  —  UN SEUL FICHIER, AUCUNE BIBLIOTHÈQUE À INSTALLER
 *
 *   /fond/1.mp3      -> joué en boucle, volume 45 %
 *   /Insultes/ (*.mp3) -> un fichier au hasard toutes les 1 à 5 min, mixé par-dessus le fond
 *
 * Câblage MAX98357A : LRC=GPIO48, BCLK(BLK)=GPIO45, DIN=GPIO47, GAIN=GND, VIN=5V, GND=GND
 * Clé USB (FAT32, table MBR ou sans partition) sur le port USB natif (OTG) via un adaptateur OTG.
 *
 * Carte Arduino : "ESP32S3 Dev Module", core esp32 >= 3.0 (Espressif)
 *   USB CDC On Boot : Disabled     (Serial = port UART ; le port natif est utilisé en host)
 *   CPU Frequency   : 240 MHz
 *
 * Contenu de ce fichier :
 *   1. réglages
 *   2. minimp3 (décodeur MP3, domaine public CC0 - https://github.com/lieff/minimp3)
 *   3. pilote USB Mass Storage (Bulk-Only / SCSI) au-dessus de l'API usb_host du core
 *   4. lecteur FAT32 en lecture seule (noms courts 8.3, insensible à la casse)
 *   5. lecteur : voix fond + voix insulte, mixage, sortie I2S
 */
#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#ifndef HOST_TEST
#include <ESP_I2S.h>
#include "usb/usb_host.h"
#endif

// ============================ 1. Réglages ============================
#define PIN_BCLK 45   // BLK
#define PIN_LRC  48   // LRC / WS
#define PIN_DIN  47   // DIN

#define OUT_RATE 44100          // fréquence I2S ; les MP3 d'une autre fréquence sont rééchantillonnés
#define BG_VOL_PCT 45           // volume du fond
#define FG_VOL_PCT 100          // volume des insultes

static const char *BG_PATH    = "/fond/1.mp3";
static const char *INSULT_DIR = "/Insultes";
static uint32_t insultMinMs = 1UL * 60UL * 1000UL;   // 1 min
static uint32_t insultMaxMs = 5UL * 60UL * 1000UL;   // 5 min

// ============================ Types communs (avant minimp3 : voir note de build) ============================
struct DirEnt {
  char name[13];       // nom court "NOM.EXT"
  uint8_t attr;
  uint32_t cluster;
  uint32_t size;
};

struct FFile {
  uint32_t first, size, pos;
  uint32_t curCl, curIdx;      // cluster courant et son index dans la chaîne
  uint8_t *chunk;              // cache de lecture (8 secteurs)
  uint32_t chunkPos, chunkLen;
};

#define FIFO_N 16384            // échantillons int16 (stéréo entrelacé), puissance de 2
#define IN_BUF 8192

struct Voice {
  void *dec;                   // mp3dec_t (défini par minimp3)
  FFile file;
  uint8_t *in;
  int inLen;
  bool fileEof, ended, active;
  uint32_t frames;
  int16_t *fifo;
  uint32_t r, w, acc;
};

#pragma GCC optimize ("O3")

// ============================ 2. minimp3 ============================
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#ifndef MINIMP3_H
#define MINIMP3_H
/*
    https://github.com/lieff/minimp3
    To the extent possible under law, the author(s) have dedicated all copyright and related and neighboring rights to this software to the public domain worldwide.
    This software is distributed without any warranty.
    See <http://creativecommons.org/publicdomain/zero/1.0/>.
*/
#include <stdint.h>

#define MINIMP3_MAX_SAMPLES_PER_FRAME (1152*2)

typedef struct
{
    int frame_bytes, frame_offset, channels, hz, layer, bitrate_kbps;
} mp3dec_frame_info_t;

typedef struct
{
    float mdct_overlap[2][9*32], qmf_state[15*2*32];
    int reserv, free_format_bytes;
    unsigned char header[4], reserv_buf[511];
} mp3dec_t;

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void mp3dec_init(mp3dec_t *dec);
#ifndef MINIMP3_FLOAT_OUTPUT
typedef int16_t mp3d_sample_t;
#else /* MINIMP3_FLOAT_OUTPUT */
typedef float mp3d_sample_t;
void mp3dec_f32_to_s16(const float *in, int16_t *out, int num_samples);
#endif /* MINIMP3_FLOAT_OUTPUT */
int mp3dec_decode_frame(mp3dec_t *dec, const uint8_t *mp3, int mp3_bytes, mp3d_sample_t *pcm, mp3dec_frame_info_t *info);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* MINIMP3_H */
#if defined(MINIMP3_IMPLEMENTATION) && !defined(_MINIMP3_IMPLEMENTATION_GUARD)
#define _MINIMP3_IMPLEMENTATION_GUARD

#include <stdlib.h>
#include <string.h>

#define MAX_FREE_FORMAT_FRAME_SIZE  2304    /* more than ISO spec's */
#ifndef MAX_FRAME_SYNC_MATCHES
#define MAX_FRAME_SYNC_MATCHES      10
#endif /* MAX_FRAME_SYNC_MATCHES */

#define MAX_L3_FRAME_PAYLOAD_BYTES  MAX_FREE_FORMAT_FRAME_SIZE /* MUST be >= 320000/8/32000*1152 = 1440 */

#define MAX_BITRESERVOIR_BYTES      511
#define SHORT_BLOCK_TYPE            2
#define STOP_BLOCK_TYPE             3
#define MODE_MONO                   3
#define MODE_JOINT_STEREO           1
#define HDR_SIZE                    4
#define HDR_IS_MONO(h)              (((h[3]) & 0xC0) == 0xC0)
#define HDR_IS_MS_STEREO(h)         (((h[3]) & 0xE0) == 0x60)
#define HDR_IS_FREE_FORMAT(h)       (((h[2]) & 0xF0) == 0)
#define HDR_IS_CRC(h)               (!((h[1]) & 1))
#define HDR_TEST_PADDING(h)         ((h[2]) & 0x2)
#define HDR_TEST_MPEG1(h)           ((h[1]) & 0x8)
#define HDR_TEST_NOT_MPEG25(h)      ((h[1]) & 0x10)
#define HDR_TEST_I_STEREO(h)        ((h[3]) & 0x10)
#define HDR_TEST_MS_STEREO(h)       ((h[3]) & 0x20)
#define HDR_GET_STEREO_MODE(h)      (((h[3]) >> 6) & 3)
#define HDR_GET_STEREO_MODE_EXT(h)  (((h[3]) >> 4) & 3)
#define HDR_GET_LAYER(h)            (((h[1]) >> 1) & 3)
#define HDR_GET_BITRATE(h)          ((h[2]) >> 4)
#define HDR_GET_SAMPLE_RATE(h)      (((h[2]) >> 2) & 3)
#define HDR_GET_MY_SAMPLE_RATE(h)   (HDR_GET_SAMPLE_RATE(h) + (((h[1] >> 3) & 1) + ((h[1] >> 4) & 1))*3)
#define HDR_IS_FRAME_576(h)         ((h[1] & 14) == 2)
#define HDR_IS_LAYER_1(h)           ((h[1] & 6) == 6)

#define BITS_DEQUANTIZER_OUT        -1
#define MAX_SCF                     (255 + BITS_DEQUANTIZER_OUT*4 - 210)
#define MAX_SCFI                    ((MAX_SCF + 3) & ~3)

#define MINIMP3_MIN(a, b)           ((a) > (b) ? (b) : (a))
#define MINIMP3_MAX(a, b)           ((a) < (b) ? (b) : (a))

#if !defined(MINIMP3_NO_SIMD)

#if !defined(MINIMP3_ONLY_SIMD) && (defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64))
/* x64 always have SSE2, arm64 always have neon, no need for generic code */
#define MINIMP3_ONLY_SIMD
#endif /* SIMD checks... */

#if (defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))) || ((defined(__i386__) || defined(__x86_64__)) && defined(__SSE2__))
#if defined(_MSC_VER)
#include <intrin.h>
#endif /* defined(_MSC_VER) */
#include <immintrin.h>
#define HAVE_SSE 1
#define HAVE_SIMD 1
#define VSTORE _mm_storeu_ps
#define VLD _mm_loadu_ps
#define VSET _mm_set1_ps
#define VADD _mm_add_ps
#define VSUB _mm_sub_ps
#define VMUL _mm_mul_ps
#define VMAC(a, x, y) _mm_add_ps(a, _mm_mul_ps(x, y))
#define VMSB(a, x, y) _mm_sub_ps(a, _mm_mul_ps(x, y))
#define VMUL_S(x, s)  _mm_mul_ps(x, _mm_set1_ps(s))
#define VREV(x) _mm_shuffle_ps(x, x, _MM_SHUFFLE(0, 1, 2, 3))
typedef __m128 f4;
#if defined(_MSC_VER) || defined(MINIMP3_ONLY_SIMD)
#define minimp3_cpuid __cpuid
#else /* defined(_MSC_VER) || defined(MINIMP3_ONLY_SIMD) */
static __inline__ __attribute__((always_inline)) void minimp3_cpuid(int CPUInfo[], const int InfoType)
{
#if defined(__PIC__)
    __asm__ __volatile__(
#if defined(__x86_64__)
        "push %%rbx\n"
        "cpuid\n"
        "xchgl %%ebx, %1\n"
        "pop  %%rbx\n"
#else /* defined(__x86_64__) */
        "xchgl %%ebx, %1\n"
        "cpuid\n"
        "xchgl %%ebx, %1\n"
#endif /* defined(__x86_64__) */
        : "=a" (CPUInfo[0]), "=r" (CPUInfo[1]), "=c" (CPUInfo[2]), "=d" (CPUInfo[3])
        : "a" (InfoType));
#else /* defined(__PIC__) */
    __asm__ __volatile__(
        "cpuid"
        : "=a" (CPUInfo[0]), "=b" (CPUInfo[1]), "=c" (CPUInfo[2]), "=d" (CPUInfo[3])
        : "a" (InfoType));
#endif /* defined(__PIC__)*/
}
#endif /* defined(_MSC_VER) || defined(MINIMP3_ONLY_SIMD) */
static int have_simd(void)
{
#ifdef MINIMP3_ONLY_SIMD
    return 1;
#else /* MINIMP3_ONLY_SIMD */
    static int g_have_simd;
    int CPUInfo[4];
#ifdef MINIMP3_TEST
    static int g_counter;
    if (g_counter++ > 100)
        return 0;
#endif /* MINIMP3_TEST */
    if (g_have_simd)
        goto end;
    minimp3_cpuid(CPUInfo, 0);
    g_have_simd = 1;
    if (CPUInfo[0] > 0)
    {
        minimp3_cpuid(CPUInfo, 1);
        g_have_simd = (CPUInfo[3] & (1 << 26)) + 1; /* SSE2 */
    }
end:
    return g_have_simd - 1;
#endif /* MINIMP3_ONLY_SIMD */
}
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define HAVE_SSE 0
#define HAVE_SIMD 1
#define VSTORE vst1q_f32
#define VLD vld1q_f32
#define VSET vmovq_n_f32
#define VADD vaddq_f32
#define VSUB vsubq_f32
#define VMUL vmulq_f32
#define VMAC(a, x, y) vmlaq_f32(a, x, y)
#define VMSB(a, x, y) vmlsq_f32(a, x, y)
#define VMUL_S(x, s)  vmulq_f32(x, vmovq_n_f32(s))
#define VREV(x) vcombine_f32(vget_high_f32(vrev64q_f32(x)), vget_low_f32(vrev64q_f32(x)))
typedef float32x4_t f4;
static int have_simd()
{   /* TODO: detect neon for !MINIMP3_ONLY_SIMD */
    return 1;
}
#else /* SIMD checks... */
#define HAVE_SSE 0
#define HAVE_SIMD 0
#ifdef MINIMP3_ONLY_SIMD
#error MINIMP3_ONLY_SIMD used, but SSE/NEON not enabled
#endif /* MINIMP3_ONLY_SIMD */
#endif /* SIMD checks... */
#else /* !defined(MINIMP3_NO_SIMD) */
#define HAVE_SIMD 0
#endif /* !defined(MINIMP3_NO_SIMD) */

#if defined(__ARM_ARCH) && (__ARM_ARCH >= 6) && !defined(__aarch64__) && !defined(_M_ARM64) && !defined(__ARM_ARCH_6M__)
#define HAVE_ARMV6 1
static __inline__ __attribute__((always_inline)) int32_t minimp3_clip_int16_arm(int32_t a)
{
    int32_t x = 0;
    __asm__ ("ssat %0, #16, %1" : "=r"(x) : "r"(a));
    return x;
}
#else
#define HAVE_ARMV6 0
#endif

typedef struct
{
    const uint8_t *buf;
    int pos, limit;
} bs_t;

typedef struct
{
    float scf[3*64];
    uint8_t total_bands, stereo_bands, bitalloc[64], scfcod[64];
} L12_scale_info;

typedef struct
{
    uint8_t tab_offset, code_tab_width, band_count;
} L12_subband_alloc_t;

typedef struct
{
    const uint8_t *sfbtab;
    uint16_t part_23_length, big_values, scalefac_compress;
    uint8_t global_gain, block_type, mixed_block_flag, n_long_sfb, n_short_sfb;
    uint8_t table_select[3], region_count[3], subblock_gain[3];
    uint8_t preflag, scalefac_scale, count1_table, scfsi;
} L3_gr_info_t;

typedef struct
{
    bs_t bs;
    uint8_t maindata[MAX_BITRESERVOIR_BYTES + MAX_L3_FRAME_PAYLOAD_BYTES];
    L3_gr_info_t gr_info[4];
    float grbuf[2][576], scf[40], syn[18 + 15][2*32];
    uint8_t ist_pos[2][39];
} mp3dec_scratch_t;

static void bs_init(bs_t *bs, const uint8_t *data, int bytes)
{
    bs->buf   = data;
    bs->pos   = 0;
    bs->limit = bytes*8;
}

static uint32_t get_bits(bs_t *bs, int n)
{
    uint32_t next, cache = 0, s = bs->pos & 7;
    int shl = n + s;
    const uint8_t *p = bs->buf + (bs->pos >> 3);
    if ((bs->pos += n) > bs->limit)
        return 0;
    next = *p++ & (255 >> s);
    while ((shl -= 8) > 0)
    {
        cache |= next << shl;
        next = *p++;
    }
    return cache | (next >> -shl);
}

static int hdr_valid(const uint8_t *h)
{
    return h[0] == 0xff &&
        ((h[1] & 0xF0) == 0xf0 || (h[1] & 0xFE) == 0xe2) &&
        (HDR_GET_LAYER(h) != 0) &&
        (HDR_GET_BITRATE(h) != 15) &&
        (HDR_GET_SAMPLE_RATE(h) != 3);
}

static int hdr_compare(const uint8_t *h1, const uint8_t *h2)
{
    return hdr_valid(h2) &&
        ((h1[1] ^ h2[1]) & 0xFE) == 0 &&
        ((h1[2] ^ h2[2]) & 0x0C) == 0 &&
        !(HDR_IS_FREE_FORMAT(h1) ^ HDR_IS_FREE_FORMAT(h2));
}

static unsigned hdr_bitrate_kbps(const uint8_t *h)
{
    static const uint8_t halfrate[2][3][15] = {
        { { 0,4,8,12,16,20,24,28,32,40,48,56,64,72,80 }, { 0,4,8,12,16,20,24,28,32,40,48,56,64,72,80 }, { 0,16,24,28,32,40,48,56,64,72,80,88,96,112,128 } },
        { { 0,16,20,24,28,32,40,48,56,64,80,96,112,128,160 }, { 0,16,24,28,32,40,48,56,64,80,96,112,128,160,192 }, { 0,16,32,48,64,80,96,112,128,144,160,176,192,208,224 } },
    };
    return 2*halfrate[!!HDR_TEST_MPEG1(h)][HDR_GET_LAYER(h) - 1][HDR_GET_BITRATE(h)];
}

static unsigned hdr_sample_rate_hz(const uint8_t *h)
{
    static const unsigned g_hz[3] = { 44100, 48000, 32000 };
    return g_hz[HDR_GET_SAMPLE_RATE(h)] >> (int)!HDR_TEST_MPEG1(h) >> (int)!HDR_TEST_NOT_MPEG25(h);
}

static unsigned hdr_frame_samples(const uint8_t *h)
{
    return HDR_IS_LAYER_1(h) ? 384 : (1152 >> (int)HDR_IS_FRAME_576(h));
}

static int hdr_frame_bytes(const uint8_t *h, int free_format_size)
{
    int frame_bytes = hdr_frame_samples(h)*hdr_bitrate_kbps(h)*125/hdr_sample_rate_hz(h);
    if (HDR_IS_LAYER_1(h))
    {
        frame_bytes &= ~3; /* slot align */
    }
    return frame_bytes ? frame_bytes : free_format_size;
}

static int hdr_padding(const uint8_t *h)
{
    return HDR_TEST_PADDING(h) ? (HDR_IS_LAYER_1(h) ? 4 : 1) : 0;
}

#ifndef MINIMP3_ONLY_MP3
static const L12_subband_alloc_t *L12_subband_alloc_table(const uint8_t *hdr, L12_scale_info *sci)
{
    const L12_subband_alloc_t *alloc;
    int mode = HDR_GET_STEREO_MODE(hdr);
    int nbands, stereo_bands = (mode == MODE_MONO) ? 0 : (mode == MODE_JOINT_STEREO) ? (HDR_GET_STEREO_MODE_EXT(hdr) << 2) + 4 : 32;

    if (HDR_IS_LAYER_1(hdr))
    {
        static const L12_subband_alloc_t g_alloc_L1[] = { { 76, 4, 32 } };
        alloc = g_alloc_L1;
        nbands = 32;
    } else if (!HDR_TEST_MPEG1(hdr))
    {
        static const L12_subband_alloc_t g_alloc_L2M2[] = { { 60, 4, 4 }, { 44, 3, 7 }, { 44, 2, 19 } };
        alloc = g_alloc_L2M2;
        nbands = 30;
    } else
    {
        static const L12_subband_alloc_t g_alloc_L2M1[] = { { 0, 4, 3 }, { 16, 4, 8 }, { 32, 3, 12 }, { 40, 2, 7 } };
        int sample_rate_idx = HDR_GET_SAMPLE_RATE(hdr);
        unsigned kbps = hdr_bitrate_kbps(hdr) >> (int)(mode != MODE_MONO);
        if (!kbps) /* free-format */
        {
            kbps = 192;
        }

        alloc = g_alloc_L2M1;
        nbands = 27;
        if (kbps < 56)
        {
            static const L12_subband_alloc_t g_alloc_L2M1_lowrate[] = { { 44, 4, 2 }, { 44, 3, 10 } };
            alloc = g_alloc_L2M1_lowrate;
            nbands = sample_rate_idx == 2 ? 12 : 8;
        } else if (kbps >= 96 && sample_rate_idx != 1)
        {
            nbands = 30;
        }
    }

    sci->total_bands = (uint8_t)nbands;
    sci->stereo_bands = (uint8_t)MINIMP3_MIN(stereo_bands, nbands);

    return alloc;
}

static void L12_read_scalefactors(bs_t *bs, uint8_t *pba, uint8_t *scfcod, int bands, float *scf)
{
    static const float g_deq_L12[18*3] = {
#define DQ(x) 9.53674316e-07f/x, 7.56931807e-07f/x, 6.00777173e-07f/x
        DQ(3),DQ(7),DQ(15),DQ(31),DQ(63),DQ(127),DQ(255),DQ(511),DQ(1023),DQ(2047),DQ(4095),DQ(8191),DQ(16383),DQ(32767),DQ(65535),DQ(3),DQ(5),DQ(9)
    };
    int i, m;
    for (i = 0; i < bands; i++)
    {
        float s = 0;
        int ba = *pba++;
        int mask = ba ? 4 + ((19 >> scfcod[i]) & 3) : 0;
        for (m = 4; m; m >>= 1)
        {
            if (mask & m)
            {
                int b = get_bits(bs, 6);
                s = g_deq_L12[ba*3 - 6 + b % 3]*(1 << 21 >> b/3);
            }
            *scf++ = s;
        }
    }
}

static void L12_read_scale_info(const uint8_t *hdr, bs_t *bs, L12_scale_info *sci)
{
    static const uint8_t g_bitalloc_code_tab[] = {
        0,17, 3, 4, 5,6,7, 8,9,10,11,12,13,14,15,16,
        0,17,18, 3,19,4,5, 6,7, 8, 9,10,11,12,13,16,
        0,17,18, 3,19,4,5,16,
        0,17,18,16,
        0,17,18,19, 4,5,6, 7,8, 9,10,11,12,13,14,15,
        0,17,18, 3,19,4,5, 6,7, 8, 9,10,11,12,13,14,
        0, 2, 3, 4, 5,6,7, 8,9,10,11,12,13,14,15,16
    };
    const L12_subband_alloc_t *subband_alloc = L12_subband_alloc_table(hdr, sci);

    int i, k = 0, ba_bits = 0;
    const uint8_t *ba_code_tab = g_bitalloc_code_tab;

    for (i = 0; i < sci->total_bands; i++)
    {
        uint8_t ba;
        if (i == k)
        {
            k += subband_alloc->band_count;
            ba_bits = subband_alloc->code_tab_width;
            ba_code_tab = g_bitalloc_code_tab + subband_alloc->tab_offset;
            subband_alloc++;
        }
        ba = ba_code_tab[get_bits(bs, ba_bits)];
        sci->bitalloc[2*i] = ba;
        if (i < sci->stereo_bands)
        {
            ba = ba_code_tab[get_bits(bs, ba_bits)];
        }
        sci->bitalloc[2*i + 1] = sci->stereo_bands ? ba : 0;
    }

    for (i = 0; i < 2*sci->total_bands; i++)
    {
        sci->scfcod[i] = sci->bitalloc[i] ? HDR_IS_LAYER_1(hdr) ? 2 : get_bits(bs, 2) : 6;
    }

    L12_read_scalefactors(bs, sci->bitalloc, sci->scfcod, sci->total_bands*2, sci->scf);

    for (i = sci->stereo_bands; i < sci->total_bands; i++)
    {
        sci->bitalloc[2*i + 1] = 0;
    }
}

static int L12_dequantize_granule(float *grbuf, bs_t *bs, L12_scale_info *sci, int group_size)
{
    int i, j, k, choff = 576;
    for (j = 0; j < 4; j++)
    {
        float *dst = grbuf + group_size*j;
        for (i = 0; i < 2*sci->total_bands; i++)
        {
            int ba = sci->bitalloc[i];
            if (ba != 0)
            {
                if (ba < 17)
                {
                    int half = (1 << (ba - 1)) - 1;
                    for (k = 0; k < group_size; k++)
                    {
                        dst[k] = (float)((int)get_bits(bs, ba) - half);
                    }
                } else
                {
                    unsigned mod = (2 << (ba - 17)) + 1;    /* 3, 5, 9 */
                    unsigned code = get_bits(bs, mod + 2 - (mod >> 3));  /* 5, 7, 10 */
                    for (k = 0; k < group_size; k++, code /= mod)
                    {
                        dst[k] = (float)((int)(code % mod - mod/2));
                    }
                }
            }
            dst += choff;
            choff = 18 - choff;
        }
    }
    return group_size*4;
}

static void L12_apply_scf_384(L12_scale_info *sci, const float *scf, float *dst)
{
    int i, k;
    memcpy(dst + 576 + sci->stereo_bands*18, dst + sci->stereo_bands*18, (sci->total_bands - sci->stereo_bands)*18*sizeof(float));
    for (i = 0; i < sci->total_bands; i++, dst += 18, scf += 6)
    {
        for (k = 0; k < 12; k++)
        {
            dst[k + 0]   *= scf[0];
            dst[k + 576] *= scf[3];
        }
    }
}
#endif /* MINIMP3_ONLY_MP3 */

static int L3_read_side_info(bs_t *bs, L3_gr_info_t *gr, const uint8_t *hdr)
{
    static const uint8_t g_scf_long[8][23] = {
        { 6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54,0 },
        { 12,12,12,12,12,12,16,20,24,28,32,40,48,56,64,76,90,2,2,2,2,2,0 },
        { 6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54,0 },
        { 6,6,6,6,6,6,8,10,12,14,16,18,22,26,32,38,46,54,62,70,76,36,0 },
        { 6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54,0 },
        { 4,4,4,4,4,4,6,6,8,8,10,12,16,20,24,28,34,42,50,54,76,158,0 },
        { 4,4,4,4,4,4,6,6,6,8,10,12,16,18,22,28,34,40,46,54,54,192,0 },
        { 4,4,4,4,4,4,6,6,8,10,12,16,20,24,30,38,46,56,68,84,102,26,0 }
    };
    static const uint8_t g_scf_short[8][40] = {
        { 4,4,4,4,4,4,4,4,4,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,30,30,30,40,40,40,18,18,18,0 },
        { 8,8,8,8,8,8,8,8,8,12,12,12,16,16,16,20,20,20,24,24,24,28,28,28,36,36,36,2,2,2,2,2,2,2,2,2,26,26,26,0 },
        { 4,4,4,4,4,4,4,4,4,6,6,6,6,6,6,8,8,8,10,10,10,14,14,14,18,18,18,26,26,26,32,32,32,42,42,42,18,18,18,0 },
        { 4,4,4,4,4,4,4,4,4,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,32,32,32,44,44,44,12,12,12,0 },
        { 4,4,4,4,4,4,4,4,4,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,30,30,30,40,40,40,18,18,18,0 },
        { 4,4,4,4,4,4,4,4,4,4,4,4,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,22,22,22,30,30,30,56,56,56,0 },
        { 4,4,4,4,4,4,4,4,4,4,4,4,6,6,6,6,6,6,10,10,10,12,12,12,14,14,14,16,16,16,20,20,20,26,26,26,66,66,66,0 },
        { 4,4,4,4,4,4,4,4,4,4,4,4,6,6,6,8,8,8,12,12,12,16,16,16,20,20,20,26,26,26,34,34,34,42,42,42,12,12,12,0 }
    };
    static const uint8_t g_scf_mixed[8][40] = {
        { 6,6,6,6,6,6,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,30,30,30,40,40,40,18,18,18,0 },
        { 12,12,12,4,4,4,8,8,8,12,12,12,16,16,16,20,20,20,24,24,24,28,28,28,36,36,36,2,2,2,2,2,2,2,2,2,26,26,26,0 },
        { 6,6,6,6,6,6,6,6,6,6,6,6,8,8,8,10,10,10,14,14,14,18,18,18,26,26,26,32,32,32,42,42,42,18,18,18,0 },
        { 6,6,6,6,6,6,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,32,32,32,44,44,44,12,12,12,0 },
        { 6,6,6,6,6,6,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,30,30,30,40,40,40,18,18,18,0 },
        { 4,4,4,4,4,4,6,6,4,4,4,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,22,22,22,30,30,30,56,56,56,0 },
        { 4,4,4,4,4,4,6,6,4,4,4,6,6,6,6,6,6,10,10,10,12,12,12,14,14,14,16,16,16,20,20,20,26,26,26,66,66,66,0 },
        { 4,4,4,4,4,4,6,6,4,4,4,6,6,6,8,8,8,12,12,12,16,16,16,20,20,20,26,26,26,34,34,34,42,42,42,12,12,12,0 }
    };

    unsigned tables, scfsi = 0;
    int main_data_begin, part_23_sum = 0;
    int sr_idx = HDR_GET_MY_SAMPLE_RATE(hdr); sr_idx -= (sr_idx != 0);
    int gr_count = HDR_IS_MONO(hdr) ? 1 : 2;

    if (HDR_TEST_MPEG1(hdr))
    {
        gr_count *= 2;
        main_data_begin = get_bits(bs, 9);
        scfsi = get_bits(bs, 7 + gr_count);
    } else
    {
        main_data_begin = get_bits(bs, 8 + gr_count) >> gr_count;
    }

    do
    {
        if (HDR_IS_MONO(hdr))
        {
            scfsi <<= 4;
        }
        gr->part_23_length = (uint16_t)get_bits(bs, 12);
        part_23_sum += gr->part_23_length;
        gr->big_values = (uint16_t)get_bits(bs,  9);
        if (gr->big_values > 288)
        {
            return -1;
        }
        gr->global_gain = (uint8_t)get_bits(bs, 8);
        gr->scalefac_compress = (uint16_t)get_bits(bs, HDR_TEST_MPEG1(hdr) ? 4 : 9);
        gr->sfbtab = g_scf_long[sr_idx];
        gr->n_long_sfb  = 22;
        gr->n_short_sfb = 0;
        if (get_bits(bs, 1))
        {
            gr->block_type = (uint8_t)get_bits(bs, 2);
            if (!gr->block_type)
            {
                return -1;
            }
            gr->mixed_block_flag = (uint8_t)get_bits(bs, 1);
            gr->region_count[0] = 7;
            gr->region_count[1] = 255;
            if (gr->block_type == SHORT_BLOCK_TYPE)
            {
                scfsi &= 0x0F0F;
                if (!gr->mixed_block_flag)
                {
                    gr->region_count[0] = 8;
                    gr->sfbtab = g_scf_short[sr_idx];
                    gr->n_long_sfb = 0;
                    gr->n_short_sfb = 39;
                } else
                {
                    gr->sfbtab = g_scf_mixed[sr_idx];
                    gr->n_long_sfb = HDR_TEST_MPEG1(hdr) ? 8 : 6;
                    gr->n_short_sfb = 30;
                }
            }
            tables = get_bits(bs, 10);
            tables <<= 5;
            gr->subblock_gain[0] = (uint8_t)get_bits(bs, 3);
            gr->subblock_gain[1] = (uint8_t)get_bits(bs, 3);
            gr->subblock_gain[2] = (uint8_t)get_bits(bs, 3);
        } else
        {
            gr->block_type = 0;
            gr->mixed_block_flag = 0;
            tables = get_bits(bs, 15);
            gr->region_count[0] = (uint8_t)get_bits(bs, 4);
            gr->region_count[1] = (uint8_t)get_bits(bs, 3);
            gr->region_count[2] = 255;
        }
        gr->table_select[0] = (uint8_t)(tables >> 10);
        gr->table_select[1] = (uint8_t)((tables >> 5) & 31);
        gr->table_select[2] = (uint8_t)((tables) & 31);
        gr->preflag = HDR_TEST_MPEG1(hdr) ? get_bits(bs, 1) : (gr->scalefac_compress >= 500);
        gr->scalefac_scale = (uint8_t)get_bits(bs, 1);
        gr->count1_table = (uint8_t)get_bits(bs, 1);
        gr->scfsi = (uint8_t)((scfsi >> 12) & 15);
        scfsi <<= 4;
        gr++;
    } while(--gr_count);

    if (part_23_sum + bs->pos > bs->limit + main_data_begin*8)
    {
        return -1;
    }

    return main_data_begin;
}

static void L3_read_scalefactors(uint8_t *scf, uint8_t *ist_pos, const uint8_t *scf_size, const uint8_t *scf_count, bs_t *bitbuf, int scfsi)
{
    int i, k;
    for (i = 0; i < 4 && scf_count[i]; i++, scfsi *= 2)
    {
        int cnt = scf_count[i];
        if (scfsi & 8)
        {
            memcpy(scf, ist_pos, cnt);
        } else
        {
            int bits = scf_size[i];
            if (!bits)
            {
                memset(scf, 0, cnt);
                memset(ist_pos, 0, cnt);
            } else
            {
                int max_scf = (scfsi < 0) ? (1 << bits) - 1 : -1;
                for (k = 0; k < cnt; k++)
                {
                    int s = get_bits(bitbuf, bits);
                    ist_pos[k] = (s == max_scf ? -1 : s);
                    scf[k] = s;
                }
            }
        }
        ist_pos += cnt;
        scf += cnt;
    }
    scf[0] = scf[1] = scf[2] = 0;
}

static float L3_ldexp_q2(float y, int exp_q2)
{
    static const float g_expfrac[4] = { 9.31322575e-10f,7.83145814e-10f,6.58544508e-10f,5.53767716e-10f };
    int e;
    do
    {
        e = MINIMP3_MIN(30*4, exp_q2);
        y *= g_expfrac[e & 3]*(1 << 30 >> (e >> 2));
    } while ((exp_q2 -= e) > 0);
    return y;
}

static void L3_decode_scalefactors(const uint8_t *hdr, uint8_t *ist_pos, bs_t *bs, const L3_gr_info_t *gr, float *scf, int ch)
{
    static const uint8_t g_scf_partitions[3][28] = {
        { 6,5,5, 5,6,5,5,5,6,5, 7,3,11,10,0,0, 7, 7, 7,0, 6, 6,6,3, 8, 8,5,0 },
        { 8,9,6,12,6,9,9,9,6,9,12,6,15,18,0,0, 6,15,12,0, 6,12,9,6, 6,18,9,0 },
        { 9,9,6,12,9,9,9,9,9,9,12,6,18,18,0,0,12,12,12,0,12, 9,9,6,15,12,9,0 }
    };
    const uint8_t *scf_partition = g_scf_partitions[!!gr->n_short_sfb + !gr->n_long_sfb];
    uint8_t scf_size[4], iscf[40];
    int i, scf_shift = gr->scalefac_scale + 1, gain_exp, scfsi = gr->scfsi;
    float gain;

    if (HDR_TEST_MPEG1(hdr))
    {
        static const uint8_t g_scfc_decode[16] = { 0,1,2,3, 12,5,6,7, 9,10,11,13, 14,15,18,19 };
        int part = g_scfc_decode[gr->scalefac_compress];
        scf_size[1] = scf_size[0] = (uint8_t)(part >> 2);
        scf_size[3] = scf_size[2] = (uint8_t)(part & 3);
    } else
    {
        static const uint8_t g_mod[6*4] = { 5,5,4,4,5,5,4,1,4,3,1,1,5,6,6,1,4,4,4,1,4,3,1,1 };
        int k, modprod, sfc, ist = HDR_TEST_I_STEREO(hdr) && ch;
        sfc = gr->scalefac_compress >> ist;
        for (k = ist*3*4; sfc >= 0; sfc -= modprod, k += 4)
        {
            for (modprod = 1, i = 3; i >= 0; i--)
            {
                scf_size[i] = (uint8_t)(sfc / modprod % g_mod[k + i]);
                modprod *= g_mod[k + i];
            }
        }
        scf_partition += k;
        scfsi = -16;
    }
    L3_read_scalefactors(iscf, ist_pos, scf_size, scf_partition, bs, scfsi);

    if (gr->n_short_sfb)
    {
        int sh = 3 - scf_shift;
        for (i = 0; i < gr->n_short_sfb; i += 3)
        {
            iscf[gr->n_long_sfb + i + 0] += gr->subblock_gain[0] << sh;
            iscf[gr->n_long_sfb + i + 1] += gr->subblock_gain[1] << sh;
            iscf[gr->n_long_sfb + i + 2] += gr->subblock_gain[2] << sh;
        }
    } else if (gr->preflag)
    {
        static const uint8_t g_preamp[10] = { 1,1,1,1,2,2,3,3,3,2 };
        for (i = 0; i < 10; i++)
        {
            iscf[11 + i] += g_preamp[i];
        }
    }

    gain_exp = gr->global_gain + BITS_DEQUANTIZER_OUT*4 - 210 - (HDR_IS_MS_STEREO(hdr) ? 2 : 0);
    gain = L3_ldexp_q2(1 << (MAX_SCFI/4),  MAX_SCFI - gain_exp);
    for (i = 0; i < (int)(gr->n_long_sfb + gr->n_short_sfb); i++)
    {
        scf[i] = L3_ldexp_q2(gain, iscf[i] << scf_shift);
    }
}

static const float g_pow43[129 + 16] = {
    0,-1,-2.519842f,-4.326749f,-6.349604f,-8.549880f,-10.902724f,-13.390518f,-16.000000f,-18.720754f,-21.544347f,-24.463781f,-27.473142f,-30.567351f,-33.741992f,-36.993181f,
    0,1,2.519842f,4.326749f,6.349604f,8.549880f,10.902724f,13.390518f,16.000000f,18.720754f,21.544347f,24.463781f,27.473142f,30.567351f,33.741992f,36.993181f,40.317474f,43.711787f,47.173345f,50.699631f,54.288352f,57.937408f,61.644865f,65.408941f,69.227979f,73.100443f,77.024898f,81.000000f,85.024491f,89.097188f,93.216975f,97.382800f,101.593667f,105.848633f,110.146801f,114.487321f,118.869381f,123.292209f,127.755065f,132.257246f,136.798076f,141.376907f,145.993119f,150.646117f,155.335327f,160.060199f,164.820202f,169.614826f,174.443577f,179.305980f,184.201575f,189.129918f,194.090580f,199.083145f,204.107210f,209.162385f,214.248292f,219.364564f,224.510845f,229.686789f,234.892058f,240.126328f,245.389280f,250.680604f,256.000000f,261.347174f,266.721841f,272.123723f,277.552547f,283.008049f,288.489971f,293.998060f,299.532071f,305.091761f,310.676898f,316.287249f,321.922592f,327.582707f,333.267377f,338.976394f,344.709550f,350.466646f,356.247482f,362.051866f,367.879608f,373.730522f,379.604427f,385.501143f,391.420496f,397.362314f,403.326427f,409.312672f,415.320884f,421.350905f,427.402579f,433.475750f,439.570269f,445.685987f,451.822757f,457.980436f,464.158883f,470.357960f,476.577530f,482.817459f,489.077615f,495.357868f,501.658090f,507.978156f,514.317941f,520.677324f,527.056184f,533.454404f,539.871867f,546.308458f,552.764065f,559.238575f,565.731879f,572.243870f,578.774440f,585.323483f,591.890898f,598.476581f,605.080431f,611.702349f,618.342238f,625.000000f,631.675540f,638.368763f,645.079578f
};

static float L3_pow_43(int x)
{
    float frac;
    int sign, mult = 256;

    if (x < 129)
    {
        return g_pow43[16 + x];
    }

    if (x < 1024)
    {
        mult = 16;
        x <<= 3;
    }

    sign = 2*x & 64;
    frac = (float)((x & 63) - sign) / ((x & ~63) + sign);
    return g_pow43[16 + ((x + sign) >> 6)]*(1.f + frac*((4.f/3) + frac*(2.f/9)))*mult;
}

static void L3_huffman(float *dst, bs_t *bs, const L3_gr_info_t *gr_info, const float *scf, int layer3gr_limit)
{
    static const int16_t tabs[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        785,785,785,785,784,784,784,784,513,513,513,513,513,513,513,513,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,
        -255,1313,1298,1282,785,785,785,785,784,784,784,784,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,290,288,
        -255,1313,1298,1282,769,769,769,769,529,529,529,529,529,529,529,529,528,528,528,528,528,528,528,528,512,512,512,512,512,512,512,512,290,288,
        -253,-318,-351,-367,785,785,785,785,784,784,784,784,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,819,818,547,547,275,275,275,275,561,560,515,546,289,274,288,258,
        -254,-287,1329,1299,1314,1312,1057,1057,1042,1042,1026,1026,784,784,784,784,529,529,529,529,529,529,529,529,769,769,769,769,768,768,768,768,563,560,306,306,291,259,
        -252,-413,-477,-542,1298,-575,1041,1041,784,784,784,784,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,-383,-399,1107,1092,1106,1061,849,849,789,789,1104,1091,773,773,1076,1075,341,340,325,309,834,804,577,577,532,532,516,516,832,818,803,816,561,561,531,531,515,546,289,289,288,258,
        -252,-429,-493,-559,1057,1057,1042,1042,529,529,529,529,529,529,529,529,784,784,784,784,769,769,769,769,512,512,512,512,512,512,512,512,-382,1077,-415,1106,1061,1104,849,849,789,789,1091,1076,1029,1075,834,834,597,581,340,340,339,324,804,833,532,532,832,772,818,803,817,787,816,771,290,290,290,290,288,258,
        -253,-349,-414,-447,-463,1329,1299,-479,1314,1312,1057,1057,1042,1042,1026,1026,785,785,785,785,784,784,784,784,769,769,769,769,768,768,768,768,-319,851,821,-335,836,850,805,849,341,340,325,336,533,533,579,579,564,564,773,832,578,548,563,516,321,276,306,291,304,259,
        -251,-572,-733,-830,-863,-879,1041,1041,784,784,784,784,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,-511,-527,-543,1396,1351,1381,1366,1395,1335,1380,-559,1334,1138,1138,1063,1063,1350,1392,1031,1031,1062,1062,1364,1363,1120,1120,1333,1348,881,881,881,881,375,374,359,373,343,358,341,325,791,791,1123,1122,-703,1105,1045,-719,865,865,790,790,774,774,1104,1029,338,293,323,308,-799,-815,833,788,772,818,803,816,322,292,307,320,561,531,515,546,289,274,288,258,
        -251,-525,-605,-685,-765,-831,-846,1298,1057,1057,1312,1282,785,785,785,785,784,784,784,784,769,769,769,769,512,512,512,512,512,512,512,512,1399,1398,1383,1367,1382,1396,1351,-511,1381,1366,1139,1139,1079,1079,1124,1124,1364,1349,1363,1333,882,882,882,882,807,807,807,807,1094,1094,1136,1136,373,341,535,535,881,775,867,822,774,-591,324,338,-671,849,550,550,866,864,609,609,293,336,534,534,789,835,773,-751,834,804,308,307,833,788,832,772,562,562,547,547,305,275,560,515,290,290,
        -252,-397,-477,-557,-622,-653,-719,-735,-750,1329,1299,1314,1057,1057,1042,1042,1312,1282,1024,1024,785,785,785,785,784,784,784,784,769,769,769,769,-383,1127,1141,1111,1126,1140,1095,1110,869,869,883,883,1079,1109,882,882,375,374,807,868,838,881,791,-463,867,822,368,263,852,837,836,-543,610,610,550,550,352,336,534,534,865,774,851,821,850,805,593,533,579,564,773,832,578,578,548,548,577,577,307,276,306,291,516,560,259,259,
        -250,-2107,-2507,-2764,-2909,-2974,-3007,-3023,1041,1041,1040,1040,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,-767,-1052,-1213,-1277,-1358,-1405,-1469,-1535,-1550,-1582,-1614,-1647,-1662,-1694,-1726,-1759,-1774,-1807,-1822,-1854,-1886,1565,-1919,-1935,-1951,-1967,1731,1730,1580,1717,-1983,1729,1564,-1999,1548,-2015,-2031,1715,1595,-2047,1714,-2063,1610,-2079,1609,-2095,1323,1323,1457,1457,1307,1307,1712,1547,1641,1700,1699,1594,1685,1625,1442,1442,1322,1322,-780,-973,-910,1279,1278,1277,1262,1276,1261,1275,1215,1260,1229,-959,974,974,989,989,-943,735,478,478,495,463,506,414,-1039,1003,958,1017,927,942,987,957,431,476,1272,1167,1228,-1183,1256,-1199,895,895,941,941,1242,1227,1212,1135,1014,1014,490,489,503,487,910,1013,985,925,863,894,970,955,1012,847,-1343,831,755,755,984,909,428,366,754,559,-1391,752,486,457,924,997,698,698,983,893,740,740,908,877,739,739,667,667,953,938,497,287,271,271,683,606,590,712,726,574,302,302,738,736,481,286,526,725,605,711,636,724,696,651,589,681,666,710,364,467,573,695,466,466,301,465,379,379,709,604,665,679,316,316,634,633,436,436,464,269,424,394,452,332,438,363,347,408,393,448,331,422,362,407,392,421,346,406,391,376,375,359,1441,1306,-2367,1290,-2383,1337,-2399,-2415,1426,1321,-2431,1411,1336,-2447,-2463,-2479,1169,1169,1049,1049,1424,1289,1412,1352,1319,-2495,1154,1154,1064,1064,1153,1153,416,390,360,404,403,389,344,374,373,343,358,372,327,357,342,311,356,326,1395,1394,1137,1137,1047,1047,1365,1392,1287,1379,1334,1364,1349,1378,1318,1363,792,792,792,792,1152,1152,1032,1032,1121,1121,1046,1046,1120,1120,1030,1030,-2895,1106,1061,1104,849,849,789,789,1091,1076,1029,1090,1060,1075,833,833,309,324,532,532,832,772,818,803,561,561,531,560,515,546,289,274,288,258,
        -250,-1179,-1579,-1836,-1996,-2124,-2253,-2333,-2413,-2477,-2542,-2574,-2607,-2622,-2655,1314,1313,1298,1312,1282,785,785,785,785,1040,1040,1025,1025,768,768,768,768,-766,-798,-830,-862,-895,-911,-927,-943,-959,-975,-991,-1007,-1023,-1039,-1055,-1070,1724,1647,-1103,-1119,1631,1767,1662,1738,1708,1723,-1135,1780,1615,1779,1599,1677,1646,1778,1583,-1151,1777,1567,1737,1692,1765,1722,1707,1630,1751,1661,1764,1614,1736,1676,1763,1750,1645,1598,1721,1691,1762,1706,1582,1761,1566,-1167,1749,1629,767,766,751,765,494,494,735,764,719,749,734,763,447,447,748,718,477,506,431,491,446,476,461,505,415,430,475,445,504,399,460,489,414,503,383,474,429,459,502,502,746,752,488,398,501,473,413,472,486,271,480,270,-1439,-1455,1357,-1471,-1487,-1503,1341,1325,-1519,1489,1463,1403,1309,-1535,1372,1448,1418,1476,1356,1462,1387,-1551,1475,1340,1447,1402,1386,-1567,1068,1068,1474,1461,455,380,468,440,395,425,410,454,364,467,466,464,453,269,409,448,268,432,1371,1473,1432,1417,1308,1460,1355,1446,1459,1431,1083,1083,1401,1416,1458,1445,1067,1067,1370,1457,1051,1051,1291,1430,1385,1444,1354,1415,1400,1443,1082,1082,1173,1113,1186,1066,1185,1050,-1967,1158,1128,1172,1097,1171,1081,-1983,1157,1112,416,266,375,400,1170,1142,1127,1065,793,793,1169,1033,1156,1096,1141,1111,1155,1080,1126,1140,898,898,808,808,897,897,792,792,1095,1152,1032,1125,1110,1139,1079,1124,882,807,838,881,853,791,-2319,867,368,263,822,852,837,866,806,865,-2399,851,352,262,534,534,821,836,594,594,549,549,593,593,533,533,848,773,579,579,564,578,548,563,276,276,577,576,306,291,516,560,305,305,275,259,
        -251,-892,-2058,-2620,-2828,-2957,-3023,-3039,1041,1041,1040,1040,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,-511,-527,-543,-559,1530,-575,-591,1528,1527,1407,1526,1391,1023,1023,1023,1023,1525,1375,1268,1268,1103,1103,1087,1087,1039,1039,1523,-604,815,815,815,815,510,495,509,479,508,463,507,447,431,505,415,399,-734,-782,1262,-815,1259,1244,-831,1258,1228,-847,-863,1196,-879,1253,987,987,748,-767,493,493,462,477,414,414,686,669,478,446,461,445,474,429,487,458,412,471,1266,1264,1009,1009,799,799,-1019,-1276,-1452,-1581,-1677,-1757,-1821,-1886,-1933,-1997,1257,1257,1483,1468,1512,1422,1497,1406,1467,1496,1421,1510,1134,1134,1225,1225,1466,1451,1374,1405,1252,1252,1358,1480,1164,1164,1251,1251,1238,1238,1389,1465,-1407,1054,1101,-1423,1207,-1439,830,830,1248,1038,1237,1117,1223,1148,1236,1208,411,426,395,410,379,269,1193,1222,1132,1235,1221,1116,976,976,1192,1162,1177,1220,1131,1191,963,963,-1647,961,780,-1663,558,558,994,993,437,408,393,407,829,978,813,797,947,-1743,721,721,377,392,844,950,828,890,706,706,812,859,796,960,948,843,934,874,571,571,-1919,690,555,689,421,346,539,539,944,779,918,873,932,842,903,888,570,570,931,917,674,674,-2575,1562,-2591,1609,-2607,1654,1322,1322,1441,1441,1696,1546,1683,1593,1669,1624,1426,1426,1321,1321,1639,1680,1425,1425,1305,1305,1545,1668,1608,1623,1667,1592,1638,1666,1320,1320,1652,1607,1409,1409,1304,1304,1288,1288,1664,1637,1395,1395,1335,1335,1622,1636,1394,1394,1319,1319,1606,1621,1392,1392,1137,1137,1137,1137,345,390,360,375,404,373,1047,-2751,-2767,-2783,1062,1121,1046,-2799,1077,-2815,1106,1061,789,789,1105,1104,263,355,310,340,325,354,352,262,339,324,1091,1076,1029,1090,1060,1075,833,833,788,788,1088,1028,818,818,803,803,561,561,531,531,816,771,546,546,289,274,288,258,
        -253,-317,-381,-446,-478,-509,1279,1279,-811,-1179,-1451,-1756,-1900,-2028,-2189,-2253,-2333,-2414,-2445,-2511,-2526,1313,1298,-2559,1041,1041,1040,1040,1025,1025,1024,1024,1022,1007,1021,991,1020,975,1019,959,687,687,1018,1017,671,671,655,655,1016,1015,639,639,758,758,623,623,757,607,756,591,755,575,754,559,543,543,1009,783,-575,-621,-685,-749,496,-590,750,749,734,748,974,989,1003,958,988,973,1002,942,987,957,972,1001,926,986,941,971,956,1000,910,985,925,999,894,970,-1071,-1087,-1102,1390,-1135,1436,1509,1451,1374,-1151,1405,1358,1480,1420,-1167,1507,1494,1389,1342,1465,1435,1450,1326,1505,1310,1493,1373,1479,1404,1492,1464,1419,428,443,472,397,736,526,464,464,486,457,442,471,484,482,1357,1449,1434,1478,1388,1491,1341,1490,1325,1489,1463,1403,1309,1477,1372,1448,1418,1433,1476,1356,1462,1387,-1439,1475,1340,1447,1402,1474,1324,1461,1371,1473,269,448,1432,1417,1308,1460,-1711,1459,-1727,1441,1099,1099,1446,1386,1431,1401,-1743,1289,1083,1083,1160,1160,1458,1445,1067,1067,1370,1457,1307,1430,1129,1129,1098,1098,268,432,267,416,266,400,-1887,1144,1187,1082,1173,1113,1186,1066,1050,1158,1128,1143,1172,1097,1171,1081,420,391,1157,1112,1170,1142,1127,1065,1169,1049,1156,1096,1141,1111,1155,1080,1126,1154,1064,1153,1140,1095,1048,-2159,1125,1110,1137,-2175,823,823,1139,1138,807,807,384,264,368,263,868,838,853,791,867,822,852,837,866,806,865,790,-2319,851,821,836,352,262,850,805,849,-2399,533,533,835,820,336,261,578,548,563,577,532,532,832,772,562,562,547,547,305,275,560,515,290,290,288,258 };
    static const uint8_t tab32[] = { 130,162,193,209,44,28,76,140,9,9,9,9,9,9,9,9,190,254,222,238,126,94,157,157,109,61,173,205 };
    static const uint8_t tab33[] = { 252,236,220,204,188,172,156,140,124,108,92,76,60,44,28,12 };
    static const int16_t tabindex[2*16] = { 0,32,64,98,0,132,180,218,292,364,426,538,648,746,0,1126,1460,1460,1460,1460,1460,1460,1460,1460,1842,1842,1842,1842,1842,1842,1842,1842 };
    static const uint8_t g_linbits[] =  { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,3,4,6,8,10,13,4,5,6,7,8,9,11,13 };

#define PEEK_BITS(n)  (bs_cache >> (32 - n))
#define FLUSH_BITS(n) { bs_cache <<= (n); bs_sh += (n); }
#define CHECK_BITS    while (bs_sh >= 0) { bs_cache |= (uint32_t)*bs_next_ptr++ << bs_sh; bs_sh -= 8; }
#define BSPOS         ((bs_next_ptr - bs->buf)*8 - 24 + bs_sh)

    float one = 0.0f;
    int ireg = 0, big_val_cnt = gr_info->big_values;
    const uint8_t *sfb = gr_info->sfbtab;
    const uint8_t *bs_next_ptr = bs->buf + bs->pos/8;
    uint32_t bs_cache = (((bs_next_ptr[0]*256u + bs_next_ptr[1])*256u + bs_next_ptr[2])*256u + bs_next_ptr[3]) << (bs->pos & 7);
    int pairs_to_decode, np, bs_sh = (bs->pos & 7) - 8;
    bs_next_ptr += 4;

    while (big_val_cnt > 0)
    {
        int tab_num = gr_info->table_select[ireg];
        int sfb_cnt = gr_info->region_count[ireg++];
        const int16_t *codebook = tabs + tabindex[tab_num];
        int linbits = g_linbits[tab_num];
        if (linbits)
        {
            do
            {
                np = *sfb++ / 2;
                pairs_to_decode = MINIMP3_MIN(big_val_cnt, np);
                one = *scf++;
                do
                {
                    int j, w = 5;
                    int leaf = codebook[PEEK_BITS(w)];
                    while (leaf < 0)
                    {
                        FLUSH_BITS(w);
                        w = leaf & 7;
                        leaf = codebook[PEEK_BITS(w) - (leaf >> 3)];
                    }
                    FLUSH_BITS(leaf >> 8);

                    for (j = 0; j < 2; j++, dst++, leaf >>= 4)
                    {
                        int lsb = leaf & 0x0F;
                        if (lsb == 15)
                        {
                            lsb += PEEK_BITS(linbits);
                            FLUSH_BITS(linbits);
                            CHECK_BITS;
                            *dst = one*L3_pow_43(lsb)*((int32_t)bs_cache < 0 ? -1: 1);
                        } else
                        {
                            *dst = g_pow43[16 + lsb - 16*(bs_cache >> 31)]*one;
                        }
                        FLUSH_BITS(lsb ? 1 : 0);
                    }
                    CHECK_BITS;
                } while (--pairs_to_decode);
            } while ((big_val_cnt -= np) > 0 && --sfb_cnt >= 0);
        } else
        {
            do
            {
                np = *sfb++ / 2;
                pairs_to_decode = MINIMP3_MIN(big_val_cnt, np);
                one = *scf++;
                do
                {
                    int j, w = 5;
                    int leaf = codebook[PEEK_BITS(w)];
                    while (leaf < 0)
                    {
                        FLUSH_BITS(w);
                        w = leaf & 7;
                        leaf = codebook[PEEK_BITS(w) - (leaf >> 3)];
                    }
                    FLUSH_BITS(leaf >> 8);

                    for (j = 0; j < 2; j++, dst++, leaf >>= 4)
                    {
                        int lsb = leaf & 0x0F;
                        *dst = g_pow43[16 + lsb - 16*(bs_cache >> 31)]*one;
                        FLUSH_BITS(lsb ? 1 : 0);
                    }
                    CHECK_BITS;
                } while (--pairs_to_decode);
            } while ((big_val_cnt -= np) > 0 && --sfb_cnt >= 0);
        }
    }

    for (np = 1 - big_val_cnt;; dst += 4)
    {
        const uint8_t *codebook_count1 = (gr_info->count1_table) ? tab33 : tab32;
        int leaf = codebook_count1[PEEK_BITS(4)];
        if (!(leaf & 8))
        {
            leaf = codebook_count1[(leaf >> 3) + (bs_cache << 4 >> (32 - (leaf & 3)))];
        }
        FLUSH_BITS(leaf & 7);
        if (BSPOS > layer3gr_limit)
        {
            break;
        }
#define RELOAD_SCALEFACTOR  if (!--np) { np = *sfb++/2; if (!np) break; one = *scf++; }
#define DEQ_COUNT1(s) if (leaf & (128 >> s)) { dst[s] = ((int32_t)bs_cache < 0) ? -one : one; FLUSH_BITS(1) }
        RELOAD_SCALEFACTOR;
        DEQ_COUNT1(0);
        DEQ_COUNT1(1);
        RELOAD_SCALEFACTOR;
        DEQ_COUNT1(2);
        DEQ_COUNT1(3);
        CHECK_BITS;
    }

    bs->pos = layer3gr_limit;
}

static void L3_midside_stereo(float *left, int n)
{
    int i = 0;
    float *right = left + 576;
#if HAVE_SIMD
    if (have_simd())
    {
        for (; i < n - 3; i += 4)
        {
            f4 vl = VLD(left + i);
            f4 vr = VLD(right + i);
            VSTORE(left + i, VADD(vl, vr));
            VSTORE(right + i, VSUB(vl, vr));
        }
#ifdef __GNUC__
        /* Workaround for spurious -Waggressive-loop-optimizations warning from gcc.
         * For more info see: https://github.com/lieff/minimp3/issues/88
         */
        if (__builtin_constant_p(n % 4 == 0) && n % 4 == 0)
            return;
#endif
    }
#endif /* HAVE_SIMD */
    for (; i < n; i++)
    {
        float a = left[i];
        float b = right[i];
        left[i] = a + b;
        right[i] = a - b;
    }
}

static void L3_intensity_stereo_band(float *left, int n, float kl, float kr)
{
    int i;
    for (i = 0; i < n; i++)
    {
        left[i + 576] = left[i]*kr;
        left[i] = left[i]*kl;
    }
}

static void L3_stereo_top_band(const float *right, const uint8_t *sfb, int nbands, int max_band[3])
{
    int i, k;

    max_band[0] = max_band[1] = max_band[2] = -1;

    for (i = 0; i < nbands; i++)
    {
        for (k = 0; k < sfb[i]; k += 2)
        {
            if (right[k] != 0 || right[k + 1] != 0)
            {
                max_band[i % 3] = i;
                break;
            }
        }
        right += sfb[i];
    }
}

static void L3_stereo_process(float *left, const uint8_t *ist_pos, const uint8_t *sfb, const uint8_t *hdr, int max_band[3], int mpeg2_sh)
{
    static const float g_pan[7*2] = { 0,1,0.21132487f,0.78867513f,0.36602540f,0.63397460f,0.5f,0.5f,0.63397460f,0.36602540f,0.78867513f,0.21132487f,1,0 };
    unsigned i, max_pos = HDR_TEST_MPEG1(hdr) ? 7 : 64;

    for (i = 0; sfb[i]; i++)
    {
        unsigned ipos = ist_pos[i];
        if ((int)i > max_band[i % 3] && ipos < max_pos)
        {
            float kl, kr, s = HDR_TEST_MS_STEREO(hdr) ? 1.41421356f : 1;
            if (HDR_TEST_MPEG1(hdr))
            {
                kl = g_pan[2*ipos];
                kr = g_pan[2*ipos + 1];
            } else
            {
                kl = 1;
                kr = L3_ldexp_q2(1, (ipos + 1) >> 1 << mpeg2_sh);
                if (ipos & 1)
                {
                    kl = kr;
                    kr = 1;
                }
            }
            L3_intensity_stereo_band(left, sfb[i], kl*s, kr*s);
        } else if (HDR_TEST_MS_STEREO(hdr))
        {
            L3_midside_stereo(left, sfb[i]);
        }
        left += sfb[i];
    }
}

static void L3_intensity_stereo(float *left, uint8_t *ist_pos, const L3_gr_info_t *gr, const uint8_t *hdr)
{
    int max_band[3], n_sfb = gr->n_long_sfb + gr->n_short_sfb;
    int i, max_blocks = gr->n_short_sfb ? 3 : 1;

    L3_stereo_top_band(left + 576, gr->sfbtab, n_sfb, max_band);
    if (gr->n_long_sfb)
    {
        max_band[0] = max_band[1] = max_band[2] = MINIMP3_MAX(MINIMP3_MAX(max_band[0], max_band[1]), max_band[2]);
    }
    for (i = 0; i < max_blocks; i++)
    {
        int default_pos = HDR_TEST_MPEG1(hdr) ? 3 : 0;
        int itop = n_sfb - max_blocks + i;
        int prev = itop - max_blocks;
        ist_pos[itop] = max_band[i] >= prev ? default_pos : ist_pos[prev];
    }
    L3_stereo_process(left, ist_pos, gr->sfbtab, hdr, max_band, gr[1].scalefac_compress & 1);
}

static void L3_reorder(float *grbuf, float *scratch, const uint8_t *sfb)
{
    int i, len;
    float *src = grbuf, *dst = scratch;

    for (;0 != (len = *sfb); sfb += 3, src += 2*len)
    {
        for (i = 0; i < len; i++, src++)
        {
            *dst++ = src[0*len];
            *dst++ = src[1*len];
            *dst++ = src[2*len];
        }
    }
    memcpy(grbuf, scratch, (dst - scratch)*sizeof(float));
}

static void L3_antialias(float *grbuf, int nbands)
{
    static const float g_aa[2][8] = {
        {0.85749293f,0.88174200f,0.94962865f,0.98331459f,0.99551782f,0.99916056f,0.99989920f,0.99999316f},
        {0.51449576f,0.47173197f,0.31337745f,0.18191320f,0.09457419f,0.04096558f,0.01419856f,0.00369997f}
    };

    for (; nbands > 0; nbands--, grbuf += 18)
    {
        int i = 0;
#if HAVE_SIMD
        if (have_simd()) for (; i < 8; i += 4)
        {
            f4 vu = VLD(grbuf + 18 + i);
            f4 vd = VLD(grbuf + 14 - i);
            f4 vc0 = VLD(g_aa[0] + i);
            f4 vc1 = VLD(g_aa[1] + i);
            vd = VREV(vd);
            VSTORE(grbuf + 18 + i, VSUB(VMUL(vu, vc0), VMUL(vd, vc1)));
            vd = VADD(VMUL(vu, vc1), VMUL(vd, vc0));
            VSTORE(grbuf + 14 - i, VREV(vd));
        }
#endif /* HAVE_SIMD */
#ifndef MINIMP3_ONLY_SIMD
        for(; i < 8; i++)
        {
            float u = grbuf[18 + i];
            float d = grbuf[17 - i];
            grbuf[18 + i] = u*g_aa[0][i] - d*g_aa[1][i];
            grbuf[17 - i] = u*g_aa[1][i] + d*g_aa[0][i];
        }
#endif /* MINIMP3_ONLY_SIMD */
    }
}

static void L3_dct3_9(float *y)
{
    float s0, s1, s2, s3, s4, s5, s6, s7, s8, t0, t2, t4;

    s0 = y[0]; s2 = y[2]; s4 = y[4]; s6 = y[6]; s8 = y[8];
    t0 = s0 + s6*0.5f;
    s0 -= s6;
    t4 = (s4 + s2)*0.93969262f;
    t2 = (s8 + s2)*0.76604444f;
    s6 = (s4 - s8)*0.17364818f;
    s4 += s8 - s2;

    s2 = s0 - s4*0.5f;
    y[4] = s4 + s0;
    s8 = t0 - t2 + s6;
    s0 = t0 - t4 + t2;
    s4 = t0 + t4 - s6;

    s1 = y[1]; s3 = y[3]; s5 = y[5]; s7 = y[7];

    s3 *= 0.86602540f;
    t0 = (s5 + s1)*0.98480775f;
    t4 = (s5 - s7)*0.34202014f;
    t2 = (s1 + s7)*0.64278761f;
    s1 = (s1 - s5 - s7)*0.86602540f;

    s5 = t0 - s3 - t2;
    s7 = t4 - s3 - t0;
    s3 = t4 + s3 - t2;

    y[0] = s4 - s7;
    y[1] = s2 + s1;
    y[2] = s0 - s3;
    y[3] = s8 + s5;
    y[5] = s8 - s5;
    y[6] = s0 + s3;
    y[7] = s2 - s1;
    y[8] = s4 + s7;
}

static void L3_imdct36(float *grbuf, float *overlap, const float *window, int nbands)
{
    int i, j;
    static const float g_twid9[18] = {
        0.73727734f,0.79335334f,0.84339145f,0.88701083f,0.92387953f,0.95371695f,0.97629601f,0.99144486f,0.99904822f,0.67559021f,0.60876143f,0.53729961f,0.46174861f,0.38268343f,0.30070580f,0.21643961f,0.13052619f,0.04361938f
    };

    for (j = 0; j < nbands; j++, grbuf += 18, overlap += 9)
    {
        float co[9], si[9];
        co[0] = -grbuf[0];
        si[0] = grbuf[17];
        for (i = 0; i < 4; i++)
        {
            si[8 - 2*i] =   grbuf[4*i + 1] - grbuf[4*i + 2];
            co[1 + 2*i] =   grbuf[4*i + 1] + grbuf[4*i + 2];
            si[7 - 2*i] =   grbuf[4*i + 4] - grbuf[4*i + 3];
            co[2 + 2*i] = -(grbuf[4*i + 3] + grbuf[4*i + 4]);
        }
        L3_dct3_9(co);
        L3_dct3_9(si);

        si[1] = -si[1];
        si[3] = -si[3];
        si[5] = -si[5];
        si[7] = -si[7];

        i = 0;

#if HAVE_SIMD
        if (have_simd()) for (; i < 8; i += 4)
        {
            f4 vovl = VLD(overlap + i);
            f4 vc = VLD(co + i);
            f4 vs = VLD(si + i);
            f4 vr0 = VLD(g_twid9 + i);
            f4 vr1 = VLD(g_twid9 + 9 + i);
            f4 vw0 = VLD(window + i);
            f4 vw1 = VLD(window + 9 + i);
            f4 vsum = VADD(VMUL(vc, vr1), VMUL(vs, vr0));
            VSTORE(overlap + i, VSUB(VMUL(vc, vr0), VMUL(vs, vr1)));
            VSTORE(grbuf + i, VSUB(VMUL(vovl, vw0), VMUL(vsum, vw1)));
            vsum = VADD(VMUL(vovl, vw1), VMUL(vsum, vw0));
            VSTORE(grbuf + 14 - i, VREV(vsum));
        }
#endif /* HAVE_SIMD */
        for (; i < 9; i++)
        {
            float ovl  = overlap[i];
            float sum  = co[i]*g_twid9[9 + i] + si[i]*g_twid9[0 + i];
            overlap[i] = co[i]*g_twid9[0 + i] - si[i]*g_twid9[9 + i];
            grbuf[i]      = ovl*window[0 + i] - sum*window[9 + i];
            grbuf[17 - i] = ovl*window[9 + i] + sum*window[0 + i];
        }
    }
}

static void L3_idct3(float x0, float x1, float x2, float *dst)
{
    float m1 = x1*0.86602540f;
    float a1 = x0 - x2*0.5f;
    dst[1] = x0 + x2;
    dst[0] = a1 + m1;
    dst[2] = a1 - m1;
}

static void L3_imdct12(float *x, float *dst, float *overlap)
{
    static const float g_twid3[6] = { 0.79335334f,0.92387953f,0.99144486f, 0.60876143f,0.38268343f,0.13052619f };
    float co[3], si[3];
    int i;

    L3_idct3(-x[0], x[6] + x[3], x[12] + x[9], co);
    L3_idct3(x[15], x[12] - x[9], x[6] - x[3], si);
    si[1] = -si[1];

    for (i = 0; i < 3; i++)
    {
        float ovl  = overlap[i];
        float sum  = co[i]*g_twid3[3 + i] + si[i]*g_twid3[0 + i];
        overlap[i] = co[i]*g_twid3[0 + i] - si[i]*g_twid3[3 + i];
        dst[i]     = ovl*g_twid3[2 - i] - sum*g_twid3[5 - i];
        dst[5 - i] = ovl*g_twid3[5 - i] + sum*g_twid3[2 - i];
    }
}

static void L3_imdct_short(float *grbuf, float *overlap, int nbands)
{
    for (;nbands > 0; nbands--, overlap += 9, grbuf += 18)
    {
        float tmp[18];
        memcpy(tmp, grbuf, sizeof(tmp));
        memcpy(grbuf, overlap, 6*sizeof(float));
        L3_imdct12(tmp, grbuf + 6, overlap + 6);
        L3_imdct12(tmp + 1, grbuf + 12, overlap + 6);
        L3_imdct12(tmp + 2, overlap, overlap + 6);
    }
}

static void L3_change_sign(float *grbuf)
{
    int b, i;
    for (b = 0, grbuf += 18; b < 32; b += 2, grbuf += 36)
        for (i = 1; i < 18; i += 2)
            grbuf[i] = -grbuf[i];
}

static void L3_imdct_gr(float *grbuf, float *overlap, unsigned block_type, unsigned n_long_bands)
{
    static const float g_mdct_window[2][18] = {
        { 0.99904822f,0.99144486f,0.97629601f,0.95371695f,0.92387953f,0.88701083f,0.84339145f,0.79335334f,0.73727734f,0.04361938f,0.13052619f,0.21643961f,0.30070580f,0.38268343f,0.46174861f,0.53729961f,0.60876143f,0.67559021f },
        { 1,1,1,1,1,1,0.99144486f,0.92387953f,0.79335334f,0,0,0,0,0,0,0.13052619f,0.38268343f,0.60876143f }
    };
    if (n_long_bands)
    {
        L3_imdct36(grbuf, overlap, g_mdct_window[0], n_long_bands);
        grbuf += 18*n_long_bands;
        overlap += 9*n_long_bands;
    }
    if (block_type == SHORT_BLOCK_TYPE)
        L3_imdct_short(grbuf, overlap, 32 - n_long_bands);
    else
        L3_imdct36(grbuf, overlap, g_mdct_window[block_type == STOP_BLOCK_TYPE], 32 - n_long_bands);
}

static void L3_save_reservoir(mp3dec_t *h, mp3dec_scratch_t *s)
{
    int pos = (s->bs.pos + 7)/8u;
    int remains = s->bs.limit/8u - pos;
    if (remains > MAX_BITRESERVOIR_BYTES)
    {
        pos += remains - MAX_BITRESERVOIR_BYTES;
        remains = MAX_BITRESERVOIR_BYTES;
    }
    if (remains > 0)
    {
        memmove(h->reserv_buf, s->maindata + pos, remains);
    }
    h->reserv = remains;
}

static int L3_restore_reservoir(mp3dec_t *h, bs_t *bs, mp3dec_scratch_t *s, int main_data_begin)
{
    int frame_bytes = (bs->limit - bs->pos)/8;
    int bytes_have = MINIMP3_MIN(h->reserv, main_data_begin);
    memcpy(s->maindata, h->reserv_buf + MINIMP3_MAX(0, h->reserv - main_data_begin), MINIMP3_MIN(h->reserv, main_data_begin));
    memcpy(s->maindata + bytes_have, bs->buf + bs->pos/8, frame_bytes);
    bs_init(&s->bs, s->maindata, bytes_have + frame_bytes);
    return h->reserv >= main_data_begin;
}

static void L3_decode(mp3dec_t *h, mp3dec_scratch_t *s, L3_gr_info_t *gr_info, int nch)
{
    int ch;

    for (ch = 0; ch < nch; ch++)
    {
        int layer3gr_limit = s->bs.pos + gr_info[ch].part_23_length;
        L3_decode_scalefactors(h->header, s->ist_pos[ch], &s->bs, gr_info + ch, s->scf, ch);
        L3_huffman(s->grbuf[ch], &s->bs, gr_info + ch, s->scf, layer3gr_limit);
    }

    if (HDR_TEST_I_STEREO(h->header))
    {
        L3_intensity_stereo(s->grbuf[0], s->ist_pos[1], gr_info, h->header);
    } else if (HDR_IS_MS_STEREO(h->header))
    {
        L3_midside_stereo(s->grbuf[0], 576);
    }

    for (ch = 0; ch < nch; ch++, gr_info++)
    {
        int aa_bands = 31;
        int n_long_bands = (gr_info->mixed_block_flag ? 2 : 0) << (int)(HDR_GET_MY_SAMPLE_RATE(h->header) == 2);

        if (gr_info->n_short_sfb)
        {
            aa_bands = n_long_bands - 1;
            L3_reorder(s->grbuf[ch] + n_long_bands*18, s->syn[0], gr_info->sfbtab + gr_info->n_long_sfb);
        }

        L3_antialias(s->grbuf[ch], aa_bands);
        L3_imdct_gr(s->grbuf[ch], h->mdct_overlap[ch], gr_info->block_type, n_long_bands);
        L3_change_sign(s->grbuf[ch]);
    }
}

static void mp3d_DCT_II(float *grbuf, int n)
{
    static const float g_sec[24] = {
        10.19000816f,0.50060302f,0.50241929f,3.40760851f,0.50547093f,0.52249861f,2.05778098f,0.51544732f,0.56694406f,1.48416460f,0.53104258f,0.64682180f,1.16943991f,0.55310392f,0.78815460f,0.97256821f,0.58293498f,1.06067765f,0.83934963f,0.62250412f,1.72244716f,0.74453628f,0.67480832f,5.10114861f
    };
    int i, k = 0;
#if HAVE_SIMD
    if (have_simd()) for (; k < n; k += 4)
    {
        f4 t[4][8], *x;
        float *y = grbuf + k;

        for (x = t[0], i = 0; i < 8; i++, x++)
        {
            f4 x0 = VLD(&y[i*18]);
            f4 x1 = VLD(&y[(15 - i)*18]);
            f4 x2 = VLD(&y[(16 + i)*18]);
            f4 x3 = VLD(&y[(31 - i)*18]);
            f4 t0 = VADD(x0, x3);
            f4 t1 = VADD(x1, x2);
            f4 t2 = VMUL_S(VSUB(x1, x2), g_sec[3*i + 0]);
            f4 t3 = VMUL_S(VSUB(x0, x3), g_sec[3*i + 1]);
            x[0] = VADD(t0, t1);
            x[8] = VMUL_S(VSUB(t0, t1), g_sec[3*i + 2]);
            x[16] = VADD(t3, t2);
            x[24] = VMUL_S(VSUB(t3, t2), g_sec[3*i + 2]);
        }
        for (x = t[0], i = 0; i < 4; i++, x += 8)
        {
            f4 x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4], x5 = x[5], x6 = x[6], x7 = x[7], xt;
            xt = VSUB(x0, x7); x0 = VADD(x0, x7);
            x7 = VSUB(x1, x6); x1 = VADD(x1, x6);
            x6 = VSUB(x2, x5); x2 = VADD(x2, x5);
            x5 = VSUB(x3, x4); x3 = VADD(x3, x4);
            x4 = VSUB(x0, x3); x0 = VADD(x0, x3);
            x3 = VSUB(x1, x2); x1 = VADD(x1, x2);
            x[0] = VADD(x0, x1);
            x[4] = VMUL_S(VSUB(x0, x1), 0.70710677f);
            x5 = VADD(x5, x6);
            x6 = VMUL_S(VADD(x6, x7), 0.70710677f);
            x7 = VADD(x7, xt);
            x3 = VMUL_S(VADD(x3, x4), 0.70710677f);
            x5 = VSUB(x5, VMUL_S(x7, 0.198912367f)); /* rotate by PI/8 */
            x7 = VADD(x7, VMUL_S(x5, 0.382683432f));
            x5 = VSUB(x5, VMUL_S(x7, 0.198912367f));
            x0 = VSUB(xt, x6); xt = VADD(xt, x6);
            x[1] = VMUL_S(VADD(xt, x7), 0.50979561f);
            x[2] = VMUL_S(VADD(x4, x3), 0.54119611f);
            x[3] = VMUL_S(VSUB(x0, x5), 0.60134488f);
            x[5] = VMUL_S(VADD(x0, x5), 0.89997619f);
            x[6] = VMUL_S(VSUB(x4, x3), 1.30656302f);
            x[7] = VMUL_S(VSUB(xt, x7), 2.56291556f);
        }

        if (k > n - 3)
        {
#if HAVE_SSE
#define VSAVE2(i, v) _mm_storel_pi((__m64 *)(void*)&y[i*18], v)
#else /* HAVE_SSE */
#define VSAVE2(i, v) vst1_f32((float32_t *)&y[i*18],  vget_low_f32(v))
#endif /* HAVE_SSE */
            for (i = 0; i < 7; i++, y += 4*18)
            {
                f4 s = VADD(t[3][i], t[3][i + 1]);
                VSAVE2(0, t[0][i]);
                VSAVE2(1, VADD(t[2][i], s));
                VSAVE2(2, VADD(t[1][i], t[1][i + 1]));
                VSAVE2(3, VADD(t[2][1 + i], s));
            }
            VSAVE2(0, t[0][7]);
            VSAVE2(1, VADD(t[2][7], t[3][7]));
            VSAVE2(2, t[1][7]);
            VSAVE2(3, t[3][7]);
        } else
        {
#define VSAVE4(i, v) VSTORE(&y[i*18], v)
            for (i = 0; i < 7; i++, y += 4*18)
            {
                f4 s = VADD(t[3][i], t[3][i + 1]);
                VSAVE4(0, t[0][i]);
                VSAVE4(1, VADD(t[2][i], s));
                VSAVE4(2, VADD(t[1][i], t[1][i + 1]));
                VSAVE4(3, VADD(t[2][1 + i], s));
            }
            VSAVE4(0, t[0][7]);
            VSAVE4(1, VADD(t[2][7], t[3][7]));
            VSAVE4(2, t[1][7]);
            VSAVE4(3, t[3][7]);
        }
    } else
#endif /* HAVE_SIMD */
#ifdef MINIMP3_ONLY_SIMD
    {} /* for HAVE_SIMD=1, MINIMP3_ONLY_SIMD=1 case we do not need non-intrinsic "else" branch */
#else /* MINIMP3_ONLY_SIMD */
    for (; k < n; k++)
    {
        float t[4][8], *x, *y = grbuf + k;

        for (x = t[0], i = 0; i < 8; i++, x++)
        {
            float x0 = y[i*18];
            float x1 = y[(15 - i)*18];
            float x2 = y[(16 + i)*18];
            float x3 = y[(31 - i)*18];
            float t0 = x0 + x3;
            float t1 = x1 + x2;
            float t2 = (x1 - x2)*g_sec[3*i + 0];
            float t3 = (x0 - x3)*g_sec[3*i + 1];
            x[0] = t0 + t1;
            x[8] = (t0 - t1)*g_sec[3*i + 2];
            x[16] = t3 + t2;
            x[24] = (t3 - t2)*g_sec[3*i + 2];
        }
        for (x = t[0], i = 0; i < 4; i++, x += 8)
        {
            float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4], x5 = x[5], x6 = x[6], x7 = x[7], xt;
            xt = x0 - x7; x0 += x7;
            x7 = x1 - x6; x1 += x6;
            x6 = x2 - x5; x2 += x5;
            x5 = x3 - x4; x3 += x4;
            x4 = x0 - x3; x0 += x3;
            x3 = x1 - x2; x1 += x2;
            x[0] = x0 + x1;
            x[4] = (x0 - x1)*0.70710677f;
            x5 =  x5 + x6;
            x6 = (x6 + x7)*0.70710677f;
            x7 =  x7 + xt;
            x3 = (x3 + x4)*0.70710677f;
            x5 -= x7*0.198912367f;  /* rotate by PI/8 */
            x7 += x5*0.382683432f;
            x5 -= x7*0.198912367f;
            x0 = xt - x6; xt += x6;
            x[1] = (xt + x7)*0.50979561f;
            x[2] = (x4 + x3)*0.54119611f;
            x[3] = (x0 - x5)*0.60134488f;
            x[5] = (x0 + x5)*0.89997619f;
            x[6] = (x4 - x3)*1.30656302f;
            x[7] = (xt - x7)*2.56291556f;

        }
        for (i = 0; i < 7; i++, y += 4*18)
        {
            y[0*18] = t[0][i];
            y[1*18] = t[2][i] + t[3][i] + t[3][i + 1];
            y[2*18] = t[1][i] + t[1][i + 1];
            y[3*18] = t[2][i + 1] + t[3][i] + t[3][i + 1];
        }
        y[0*18] = t[0][7];
        y[1*18] = t[2][7] + t[3][7];
        y[2*18] = t[1][7];
        y[3*18] = t[3][7];
    }
#endif /* MINIMP3_ONLY_SIMD */
}

#ifndef MINIMP3_FLOAT_OUTPUT
static int16_t mp3d_scale_pcm(float sample)
{
#if HAVE_ARMV6
    int32_t s32 = (int32_t)(sample + .5f);
    s32 -= (s32 < 0);
    int16_t s = (int16_t)minimp3_clip_int16_arm(s32);
#else
    if (sample >=  32766.5) return (int16_t) 32767;
    if (sample <= -32767.5) return (int16_t)-32768;
    int16_t s = (int16_t)(sample + .5f);
    s -= (s < 0);   /* away from zero, to be compliant */
#endif
    return s;
}
#else /* MINIMP3_FLOAT_OUTPUT */
static float mp3d_scale_pcm(float sample)
{
    return sample*(1.f/32768.f);
}
#endif /* MINIMP3_FLOAT_OUTPUT */

static void mp3d_synth_pair(mp3d_sample_t *pcm, int nch, const float *z)
{
    float a;
    a  = (z[14*64] - z[    0]) * 29;
    a += (z[ 1*64] + z[13*64]) * 213;
    a += (z[12*64] - z[ 2*64]) * 459;
    a += (z[ 3*64] + z[11*64]) * 2037;
    a += (z[10*64] - z[ 4*64]) * 5153;
    a += (z[ 5*64] + z[ 9*64]) * 6574;
    a += (z[ 8*64] - z[ 6*64]) * 37489;
    a +=  z[ 7*64]             * 75038;
    pcm[0] = mp3d_scale_pcm(a);

    z += 2;
    a  = z[14*64] * 104;
    a += z[12*64] * 1567;
    a += z[10*64] * 9727;
    a += z[ 8*64] * 64019;
    a += z[ 6*64] * -9975;
    a += z[ 4*64] * -45;
    a += z[ 2*64] * 146;
    a += z[ 0*64] * -5;
    pcm[16*nch] = mp3d_scale_pcm(a);
}

static void mp3d_synth(float *xl, mp3d_sample_t *dstl, int nch, float *lins)
{
    int i;
    float *xr = xl + 576*(nch - 1);
    mp3d_sample_t *dstr = dstl + (nch - 1);

    static const float g_win[] = {
        -1,26,-31,208,218,401,-519,2063,2000,4788,-5517,7134,5959,35640,-39336,74992,
        -1,24,-35,202,222,347,-581,2080,1952,4425,-5879,7640,5288,33791,-41176,74856,
        -1,21,-38,196,225,294,-645,2087,1893,4063,-6237,8092,4561,31947,-43006,74630,
        -1,19,-41,190,227,244,-711,2085,1822,3705,-6589,8492,3776,30112,-44821,74313,
        -1,17,-45,183,228,197,-779,2075,1739,3351,-6935,8840,2935,28289,-46617,73908,
        -1,16,-49,176,228,153,-848,2057,1644,3004,-7271,9139,2037,26482,-48390,73415,
        -2,14,-53,169,227,111,-919,2032,1535,2663,-7597,9389,1082,24694,-50137,72835,
        -2,13,-58,161,224,72,-991,2001,1414,2330,-7910,9592,70,22929,-51853,72169,
        -2,11,-63,154,221,36,-1064,1962,1280,2006,-8209,9750,-998,21189,-53534,71420,
        -2,10,-68,147,215,2,-1137,1919,1131,1692,-8491,9863,-2122,19478,-55178,70590,
        -3,9,-73,139,208,-29,-1210,1870,970,1388,-8755,9935,-3300,17799,-56778,69679,
        -3,8,-79,132,200,-57,-1283,1817,794,1095,-8998,9966,-4533,16155,-58333,68692,
        -4,7,-85,125,189,-83,-1356,1759,605,814,-9219,9959,-5818,14548,-59838,67629,
        -4,7,-91,117,177,-106,-1428,1698,402,545,-9416,9916,-7154,12980,-61289,66494,
        -5,6,-97,111,163,-127,-1498,1634,185,288,-9585,9838,-8540,11455,-62684,65290
    };
    float *zlin = lins + 15*64;
    const float *w = g_win;

    zlin[4*15]     = xl[18*16];
    zlin[4*15 + 1] = xr[18*16];
    zlin[4*15 + 2] = xl[0];
    zlin[4*15 + 3] = xr[0];

    zlin[4*31]     = xl[1 + 18*16];
    zlin[4*31 + 1] = xr[1 + 18*16];
    zlin[4*31 + 2] = xl[1];
    zlin[4*31 + 3] = xr[1];

    mp3d_synth_pair(dstr, nch, lins + 4*15 + 1);
    mp3d_synth_pair(dstr + 32*nch, nch, lins + 4*15 + 64 + 1);
    mp3d_synth_pair(dstl, nch, lins + 4*15);
    mp3d_synth_pair(dstl + 32*nch, nch, lins + 4*15 + 64);

#if HAVE_SIMD
    if (have_simd()) for (i = 14; i >= 0; i--)
    {
#define VLOAD(k) f4 w0 = VSET(*w++); f4 w1 = VSET(*w++); f4 vz = VLD(&zlin[4*i - 64*k]); f4 vy = VLD(&zlin[4*i - 64*(15 - k)]);
#define V0(k) { VLOAD(k) b =         VADD(VMUL(vz, w1), VMUL(vy, w0)) ; a =         VSUB(VMUL(vz, w0), VMUL(vy, w1));  }
#define V1(k) { VLOAD(k) b = VADD(b, VADD(VMUL(vz, w1), VMUL(vy, w0))); a = VADD(a, VSUB(VMUL(vz, w0), VMUL(vy, w1))); }
#define V2(k) { VLOAD(k) b = VADD(b, VADD(VMUL(vz, w1), VMUL(vy, w0))); a = VADD(a, VSUB(VMUL(vy, w1), VMUL(vz, w0))); }
        f4 a, b;
        zlin[4*i]     = xl[18*(31 - i)];
        zlin[4*i + 1] = xr[18*(31 - i)];
        zlin[4*i + 2] = xl[1 + 18*(31 - i)];
        zlin[4*i + 3] = xr[1 + 18*(31 - i)];
        zlin[4*i + 64] = xl[1 + 18*(1 + i)];
        zlin[4*i + 64 + 1] = xr[1 + 18*(1 + i)];
        zlin[4*i - 64 + 2] = xl[18*(1 + i)];
        zlin[4*i - 64 + 3] = xr[18*(1 + i)];

        V0(0) V2(1) V1(2) V2(3) V1(4) V2(5) V1(6) V2(7)

        {
#ifndef MINIMP3_FLOAT_OUTPUT
#if HAVE_SSE
            static const f4 g_max = { 32767.0f, 32767.0f, 32767.0f, 32767.0f };
            static const f4 g_min = { -32768.0f, -32768.0f, -32768.0f, -32768.0f };
            __m128i pcm8 = _mm_packs_epi32(_mm_cvtps_epi32(_mm_max_ps(_mm_min_ps(a, g_max), g_min)),
                                           _mm_cvtps_epi32(_mm_max_ps(_mm_min_ps(b, g_max), g_min)));
            dstr[(15 - i)*nch] = _mm_extract_epi16(pcm8, 1);
            dstr[(17 + i)*nch] = _mm_extract_epi16(pcm8, 5);
            dstl[(15 - i)*nch] = _mm_extract_epi16(pcm8, 0);
            dstl[(17 + i)*nch] = _mm_extract_epi16(pcm8, 4);
            dstr[(47 - i)*nch] = _mm_extract_epi16(pcm8, 3);
            dstr[(49 + i)*nch] = _mm_extract_epi16(pcm8, 7);
            dstl[(47 - i)*nch] = _mm_extract_epi16(pcm8, 2);
            dstl[(49 + i)*nch] = _mm_extract_epi16(pcm8, 6);
#else /* HAVE_SSE */
            int16x4_t pcma, pcmb;
            a = VADD(a, VSET(0.5f));
            b = VADD(b, VSET(0.5f));
            pcma = vqmovn_s32(vqaddq_s32(vcvtq_s32_f32(a), vreinterpretq_s32_u32(vcltq_f32(a, VSET(0)))));
            pcmb = vqmovn_s32(vqaddq_s32(vcvtq_s32_f32(b), vreinterpretq_s32_u32(vcltq_f32(b, VSET(0)))));
            vst1_lane_s16(dstr + (15 - i)*nch, pcma, 1);
            vst1_lane_s16(dstr + (17 + i)*nch, pcmb, 1);
            vst1_lane_s16(dstl + (15 - i)*nch, pcma, 0);
            vst1_lane_s16(dstl + (17 + i)*nch, pcmb, 0);
            vst1_lane_s16(dstr + (47 - i)*nch, pcma, 3);
            vst1_lane_s16(dstr + (49 + i)*nch, pcmb, 3);
            vst1_lane_s16(dstl + (47 - i)*nch, pcma, 2);
            vst1_lane_s16(dstl + (49 + i)*nch, pcmb, 2);
#endif /* HAVE_SSE */

#else /* MINIMP3_FLOAT_OUTPUT */

            static const f4 g_scale = { 1.0f/32768.0f, 1.0f/32768.0f, 1.0f/32768.0f, 1.0f/32768.0f };
            a = VMUL(a, g_scale);
            b = VMUL(b, g_scale);
#if HAVE_SSE
            _mm_store_ss(dstr + (15 - i)*nch, _mm_shuffle_ps(a, a, _MM_SHUFFLE(1, 1, 1, 1)));
            _mm_store_ss(dstr + (17 + i)*nch, _mm_shuffle_ps(b, b, _MM_SHUFFLE(1, 1, 1, 1)));
            _mm_store_ss(dstl + (15 - i)*nch, _mm_shuffle_ps(a, a, _MM_SHUFFLE(0, 0, 0, 0)));
            _mm_store_ss(dstl + (17 + i)*nch, _mm_shuffle_ps(b, b, _MM_SHUFFLE(0, 0, 0, 0)));
            _mm_store_ss(dstr + (47 - i)*nch, _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 3, 3, 3)));
            _mm_store_ss(dstr + (49 + i)*nch, _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 3, 3, 3)));
            _mm_store_ss(dstl + (47 - i)*nch, _mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 2, 2, 2)));
            _mm_store_ss(dstl + (49 + i)*nch, _mm_shuffle_ps(b, b, _MM_SHUFFLE(2, 2, 2, 2)));
#else /* HAVE_SSE */
            vst1q_lane_f32(dstr + (15 - i)*nch, a, 1);
            vst1q_lane_f32(dstr + (17 + i)*nch, b, 1);
            vst1q_lane_f32(dstl + (15 - i)*nch, a, 0);
            vst1q_lane_f32(dstl + (17 + i)*nch, b, 0);
            vst1q_lane_f32(dstr + (47 - i)*nch, a, 3);
            vst1q_lane_f32(dstr + (49 + i)*nch, b, 3);
            vst1q_lane_f32(dstl + (47 - i)*nch, a, 2);
            vst1q_lane_f32(dstl + (49 + i)*nch, b, 2);
#endif /* HAVE_SSE */
#endif /* MINIMP3_FLOAT_OUTPUT */
        }
    } else
#endif /* HAVE_SIMD */
#ifdef MINIMP3_ONLY_SIMD
    {} /* for HAVE_SIMD=1, MINIMP3_ONLY_SIMD=1 case we do not need non-intrinsic "else" branch */
#else /* MINIMP3_ONLY_SIMD */
    for (i = 14; i >= 0; i--)
    {
#define LOAD(k) float w0 = *w++; float w1 = *w++; float *vz = &zlin[4*i - k*64]; float *vy = &zlin[4*i - (15 - k)*64];
#define S0(k) { int j; LOAD(k); for (j = 0; j < 4; j++) b[j]  = vz[j]*w1 + vy[j]*w0, a[j]  = vz[j]*w0 - vy[j]*w1; }
#define S1(k) { int j; LOAD(k); for (j = 0; j < 4; j++) b[j] += vz[j]*w1 + vy[j]*w0, a[j] += vz[j]*w0 - vy[j]*w1; }
#define S2(k) { int j; LOAD(k); for (j = 0; j < 4; j++) b[j] += vz[j]*w1 + vy[j]*w0, a[j] += vy[j]*w1 - vz[j]*w0; }
        float a[4], b[4];

        zlin[4*i]     = xl[18*(31 - i)];
        zlin[4*i + 1] = xr[18*(31 - i)];
        zlin[4*i + 2] = xl[1 + 18*(31 - i)];
        zlin[4*i + 3] = xr[1 + 18*(31 - i)];
        zlin[4*(i + 16)]   = xl[1 + 18*(1 + i)];
        zlin[4*(i + 16) + 1] = xr[1 + 18*(1 + i)];
        zlin[4*(i - 16) + 2] = xl[18*(1 + i)];
        zlin[4*(i - 16) + 3] = xr[18*(1 + i)];

        S0(0) S2(1) S1(2) S2(3) S1(4) S2(5) S1(6) S2(7)

        dstr[(15 - i)*nch] = mp3d_scale_pcm(a[1]);
        dstr[(17 + i)*nch] = mp3d_scale_pcm(b[1]);
        dstl[(15 - i)*nch] = mp3d_scale_pcm(a[0]);
        dstl[(17 + i)*nch] = mp3d_scale_pcm(b[0]);
        dstr[(47 - i)*nch] = mp3d_scale_pcm(a[3]);
        dstr[(49 + i)*nch] = mp3d_scale_pcm(b[3]);
        dstl[(47 - i)*nch] = mp3d_scale_pcm(a[2]);
        dstl[(49 + i)*nch] = mp3d_scale_pcm(b[2]);
    }
#endif /* MINIMP3_ONLY_SIMD */
}

static void mp3d_synth_granule(float *qmf_state, float *grbuf, int nbands, int nch, mp3d_sample_t *pcm, float *lins)
{
    int i;
    for (i = 0; i < nch; i++)
    {
        mp3d_DCT_II(grbuf + 576*i, nbands);
    }

    memcpy(lins, qmf_state, sizeof(float)*15*64);

    for (i = 0; i < nbands; i += 2)
    {
        mp3d_synth(grbuf + i, pcm + 32*nch*i, nch, lins + i*64);
    }
#ifndef MINIMP3_NONSTANDARD_BUT_LOGICAL
    if (nch == 1)
    {
        for (i = 0; i < 15*64; i += 2)
        {
            qmf_state[i] = lins[nbands*64 + i];
        }
    } else
#endif /* MINIMP3_NONSTANDARD_BUT_LOGICAL */
    {
        memcpy(qmf_state, lins + nbands*64, sizeof(float)*15*64);
    }
}

static int mp3d_match_frame(const uint8_t *hdr, int mp3_bytes, int frame_bytes)
{
    int i, nmatch;
    for (i = 0, nmatch = 0; nmatch < MAX_FRAME_SYNC_MATCHES; nmatch++)
    {
        i += hdr_frame_bytes(hdr + i, frame_bytes) + hdr_padding(hdr + i);
        if (i + HDR_SIZE > mp3_bytes)
            return nmatch > 0;
        if (!hdr_compare(hdr, hdr + i))
            return 0;
    }
    return 1;
}

static int mp3d_find_frame(const uint8_t *mp3, int mp3_bytes, int *free_format_bytes, int *ptr_frame_bytes)
{
    int i, k;
    for (i = 0; i < mp3_bytes - HDR_SIZE; i++, mp3++)
    {
        if (hdr_valid(mp3))
        {
            int frame_bytes = hdr_frame_bytes(mp3, *free_format_bytes);
            int frame_and_padding = frame_bytes + hdr_padding(mp3);

            for (k = HDR_SIZE; !frame_bytes && k < MAX_FREE_FORMAT_FRAME_SIZE && i + 2*k < mp3_bytes - HDR_SIZE; k++)
            {
                if (hdr_compare(mp3, mp3 + k))
                {
                    int fb = k - hdr_padding(mp3);
                    int nextfb = fb + hdr_padding(mp3 + k);
                    if (i + k + nextfb + HDR_SIZE > mp3_bytes || !hdr_compare(mp3, mp3 + k + nextfb))
                        continue;
                    frame_and_padding = k;
                    frame_bytes = fb;
                    *free_format_bytes = fb;
                }
            }
            if ((frame_bytes && i + frame_and_padding <= mp3_bytes &&
                mp3d_match_frame(mp3, mp3_bytes - i, frame_bytes)) ||
                (!i && frame_and_padding == mp3_bytes))
            {
                *ptr_frame_bytes = frame_and_padding;
                return i;
            }
            *free_format_bytes = 0;
        }
    }
    *ptr_frame_bytes = 0;
    return mp3_bytes;
}

void mp3dec_init(mp3dec_t *dec)
{
    dec->header[0] = 0;
}

int mp3dec_decode_frame(mp3dec_t *dec, const uint8_t *mp3, int mp3_bytes, mp3d_sample_t *pcm, mp3dec_frame_info_t *info)
{
    int i = 0, igr, frame_size = 0, success = 1;
    const uint8_t *hdr;
    bs_t bs_frame[1];
    mp3dec_scratch_t scratch;

    if (mp3_bytes > 4 && dec->header[0] == 0xff && hdr_compare(dec->header, mp3))
    {
        frame_size = hdr_frame_bytes(mp3, dec->free_format_bytes) + hdr_padding(mp3);
        if (frame_size != mp3_bytes && (frame_size + HDR_SIZE > mp3_bytes || !hdr_compare(mp3, mp3 + frame_size)))
        {
            frame_size = 0;
        }
    }
    if (!frame_size)
    {
        memset(dec, 0, sizeof(mp3dec_t));
        i = mp3d_find_frame(mp3, mp3_bytes, &dec->free_format_bytes, &frame_size);
        if (!frame_size || i + frame_size > mp3_bytes)
        {
            info->frame_bytes = i;
            return 0;
        }
    }

    hdr = mp3 + i;
    memcpy(dec->header, hdr, HDR_SIZE);
    info->frame_bytes = i + frame_size;
    info->frame_offset = i;
    info->channels = HDR_IS_MONO(hdr) ? 1 : 2;
    info->hz = hdr_sample_rate_hz(hdr);
    info->layer = 4 - HDR_GET_LAYER(hdr);
    info->bitrate_kbps = hdr_bitrate_kbps(hdr);

    if (!pcm)
    {
        return hdr_frame_samples(hdr);
    }

    bs_init(bs_frame, hdr + HDR_SIZE, frame_size - HDR_SIZE);
    if (HDR_IS_CRC(hdr))
    {
        get_bits(bs_frame, 16);
    }

    if (info->layer == 3)
    {
        int main_data_begin = L3_read_side_info(bs_frame, scratch.gr_info, hdr);
        if (main_data_begin < 0 || bs_frame->pos > bs_frame->limit)
        {
            mp3dec_init(dec);
            return 0;
        }
        success = L3_restore_reservoir(dec, bs_frame, &scratch, main_data_begin);
        if (success)
        {
            for (igr = 0; igr < (HDR_TEST_MPEG1(hdr) ? 2 : 1); igr++, pcm += 576*info->channels)
            {
                memset(scratch.grbuf[0], 0, 576*2*sizeof(float));
                L3_decode(dec, &scratch, scratch.gr_info + igr*info->channels, info->channels);
                mp3d_synth_granule(dec->qmf_state, scratch.grbuf[0], 18, info->channels, pcm, scratch.syn[0]);
            }
        }
        L3_save_reservoir(dec, &scratch);
    } else
    {
#ifdef MINIMP3_ONLY_MP3
        return 0;
#else /* MINIMP3_ONLY_MP3 */
        L12_scale_info sci[1];
        L12_read_scale_info(hdr, bs_frame, sci);

        memset(scratch.grbuf[0], 0, 576*2*sizeof(float));
        for (i = 0, igr = 0; igr < 3; igr++)
        {
            if (12 == (i += L12_dequantize_granule(scratch.grbuf[0] + i, bs_frame, sci, info->layer | 1)))
            {
                i = 0;
                L12_apply_scf_384(sci, sci->scf + igr, scratch.grbuf[0]);
                mp3d_synth_granule(dec->qmf_state, scratch.grbuf[0], 12, info->channels, pcm, scratch.syn[0]);
                memset(scratch.grbuf[0], 0, 576*2*sizeof(float));
                pcm += 384*info->channels;
            }
            if (bs_frame->pos > bs_frame->limit)
            {
                mp3dec_init(dec);
                return 0;
            }
        }
#endif /* MINIMP3_ONLY_MP3 */
    }
    return success*hdr_frame_samples(dec->header);
}

#ifdef MINIMP3_FLOAT_OUTPUT
void mp3dec_f32_to_s16(const float *in, int16_t *out, int num_samples)
{
    int i = 0;
#if HAVE_SIMD
    int aligned_count = num_samples & ~7;
    for(; i < aligned_count; i += 8)
    {
        static const f4 g_scale = { 32768.0f, 32768.0f, 32768.0f, 32768.0f };
        f4 a = VMUL(VLD(&in[i  ]), g_scale);
        f4 b = VMUL(VLD(&in[i+4]), g_scale);
#if HAVE_SSE
        static const f4 g_max = { 32767.0f, 32767.0f, 32767.0f, 32767.0f };
        static const f4 g_min = { -32768.0f, -32768.0f, -32768.0f, -32768.0f };
        __m128i pcm8 = _mm_packs_epi32(_mm_cvtps_epi32(_mm_max_ps(_mm_min_ps(a, g_max), g_min)),
                                       _mm_cvtps_epi32(_mm_max_ps(_mm_min_ps(b, g_max), g_min)));
        out[i  ] = _mm_extract_epi16(pcm8, 0);
        out[i+1] = _mm_extract_epi16(pcm8, 1);
        out[i+2] = _mm_extract_epi16(pcm8, 2);
        out[i+3] = _mm_extract_epi16(pcm8, 3);
        out[i+4] = _mm_extract_epi16(pcm8, 4);
        out[i+5] = _mm_extract_epi16(pcm8, 5);
        out[i+6] = _mm_extract_epi16(pcm8, 6);
        out[i+7] = _mm_extract_epi16(pcm8, 7);
#else /* HAVE_SSE */
        int16x4_t pcma, pcmb;
        a = VADD(a, VSET(0.5f));
        b = VADD(b, VSET(0.5f));
        pcma = vqmovn_s32(vqaddq_s32(vcvtq_s32_f32(a), vreinterpretq_s32_u32(vcltq_f32(a, VSET(0)))));
        pcmb = vqmovn_s32(vqaddq_s32(vcvtq_s32_f32(b), vreinterpretq_s32_u32(vcltq_f32(b, VSET(0)))));
        vst1_lane_s16(out+i  , pcma, 0);
        vst1_lane_s16(out+i+1, pcma, 1);
        vst1_lane_s16(out+i+2, pcma, 2);
        vst1_lane_s16(out+i+3, pcma, 3);
        vst1_lane_s16(out+i+4, pcmb, 0);
        vst1_lane_s16(out+i+5, pcmb, 1);
        vst1_lane_s16(out+i+6, pcmb, 2);
        vst1_lane_s16(out+i+7, pcmb, 3);
#endif /* HAVE_SSE */
    }
#endif /* HAVE_SIMD */
    for(; i < num_samples; i++)
    {
        float sample = in[i] * 32768.0f;
        if (sample >=  32766.5)
            out[i] = (int16_t) 32767;
        else if (sample <= -32767.5)
            out[i] = (int16_t)-32768;
        else
        {
            int16_t s = (int16_t)(sample + .5f);
            s -= (s < 0);   /* away from zero, to be compliant */
            out[i] = s;
        }
    }
}
#endif /* MINIMP3_FLOAT_OUTPUT */
#endif /* MINIMP3_IMPLEMENTATION && !_MINIMP3_IMPLEMENTATION_GUARD */

// ============================ 3. USB Mass Storage (host) ============================
static inline uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

#ifndef HOST_TEST

#define XFER_MAX 4096                       // 8 secteurs par lecture

static usb_host_client_handle_t uClient;
static usb_device_handle_t uDev;
static usb_transfer_t *uXfer, *uCtrl;
static SemaphoreHandle_t uSem;
static volatile bool uNew = false, uGone = false;
static volatile uint8_t uNewAddr = 0;
static bool uAttached = false, uReady = false;
static uint8_t uIf, uEpIn, uEpOut;
static uint16_t uInMps = 64, uOutMps = 64;
static uint32_t cbwTag = 1;

struct __attribute__((packed)) CBW {
  uint32_t sig, tag, len;
  uint8_t flags, lun, cbLen, cb[16];
};

static void uClientCb(const usb_host_client_event_msg_t *msg, void *) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (!uAttached) { uNewAddr = msg->new_dev.address; uNew = true; }
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    if (uAttached && msg->dev_gone.dev_hdl == uDev) uGone = true;
  }
}

static void uXferCb(usb_transfer_t *) { xSemaphoreGive(uSem); }

static void usbLibTask(void *) {
  for (;;) { uint32_t flags; usb_host_lib_handle_events(portMAX_DELAY, &flags); }
}
static void usbClientTask(void *) {
  for (;;) usb_host_client_handle_events(uClient, portMAX_DELAY);
}

// Transfert bulk. 0 = ok, 1 = STALL (endpoint remis en service), -1 = erreur
static int bulk(uint8_t ep, uint8_t *data, uint32_t len, uint32_t *actual) {
  bool in = ep & 0x80;
  uint32_t mps = in ? uInMps : uOutMps;
  uint32_t n = in ? ((len + mps - 1) / mps) * mps : len;
  if (n > XFER_MAX + 64) return -1;
  if (!in) memcpy(uXfer->data_buffer, data, len);
  uXfer->device_handle = uDev;
  uXfer->bEndpointAddress = ep;
  uXfer->num_bytes = n;
  uXfer->callback = uXferCb;
  uXfer->context = nullptr;
  uXfer->timeout_ms = 0;
  while (xSemaphoreTake(uSem, 0) == pdTRUE) {}
  if (usb_host_transfer_submit(uXfer) != ESP_OK) return -1;
  if (xSemaphoreTake(uSem, pdMS_TO_TICKS(4000)) != pdTRUE) {
    usb_host_endpoint_halt(uDev, ep);
    usb_host_endpoint_flush(uDev, ep);
    xSemaphoreTake(uSem, pdMS_TO_TICKS(1000));
    usb_host_endpoint_clear(uDev, ep);
    return -1;
  }
  if (uXfer->status == USB_TRANSFER_STATUS_STALL) { usb_host_endpoint_clear(uDev, ep); return 1; }
  if (uXfer->status != USB_TRANSFER_STATUS_COMPLETED) return -1;
  uint32_t got = uXfer->actual_num_bytes;
  if (in) memcpy(data, uXfer->data_buffer, umin(got, len));
  if (actual) *actual = got;
  return 0;
}

// Commande SCSI via Bulk-Only Transport. 0 = ok, 1 = commande échouée, -1 = erreur de transport
static int botCmd(const uint8_t *cdb, uint8_t cbLen, bool in, uint8_t *data, uint32_t len, uint32_t *got) {
  CBW cbw;
  memset(&cbw, 0, sizeof(cbw));
  cbw.sig = 0x43425355;
  cbw.tag = cbwTag++;
  cbw.len = len;
  cbw.flags = in ? 0x80 : 0x00;
  cbw.cbLen = cbLen;
  memcpy(cbw.cb, cdb, cbLen);
  if (bulk(uEpOut, (uint8_t *)&cbw, 31, nullptr) != 0) return -1;
  uint32_t a = 0;
  bool dataStall = false;
  if (len) {
    int r = bulk(in ? uEpIn : uEpOut, data, len, &a);
    if (r < 0) return -1;
    dataStall = (r == 1);
  }
  uint8_t csw[16];
  uint32_t cg = 0;
  int r = bulk(uEpIn, csw, 13, &cg);
  if (r == 1) r = bulk(uEpIn, csw, 13, &cg);
  if (r != 0 || cg != 13) return -1;
  if (rd32(csw) != 0x53425355 || rd32(csw + 4) != cbw.tag) return -1;
  if (got) *got = a;
  if (csw[12] == 0) return dataStall ? 1 : 0;
  return csw[12] == 1 ? 1 : -1;
}

static void botReset() {            // Bulk-Only Mass Storage Reset + fin des STALL
  usb_setup_packet_t *s = (usb_setup_packet_t *)uCtrl->data_buffer;
  s->bmRequestType = 0x21; s->bRequest = 0xFF; s->wValue = 0; s->wIndex = uIf; s->wLength = 0;
  uCtrl->device_handle = uDev;
  uCtrl->bEndpointAddress = 0;
  uCtrl->num_bytes = sizeof(usb_setup_packet_t);
  uCtrl->callback = uXferCb;
  uCtrl->context = nullptr;
  uCtrl->timeout_ms = 0;
  while (xSemaphoreTake(uSem, 0) == pdTRUE) {}
  if (usb_host_transfer_submit_control(uClient, uCtrl) == ESP_OK) xSemaphoreTake(uSem, pdMS_TO_TICKS(2000));
  usb_host_endpoint_clear(uDev, uEpIn);
  usb_host_endpoint_clear(uDev, uEpOut);
}

static void scsiSense() {
  uint8_t c[6] = {0x03, 0, 0, 0, 18, 0};
  uint8_t d[18];
  botCmd(c, 6, true, d, 18, nullptr);
}

// Lit n secteurs de 512 octets (n <= 8)
static bool usbReadSectors(uint32_t lba, uint32_t n, uint8_t *buf) {
  for (int tries = 0; tries < 3; tries++) {
    uint8_t c[10] = {0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                     0, (uint8_t)(n >> 8), (uint8_t)n, 0};
    uint32_t g = 0;
    int r = botCmd(c, 10, true, buf, n * 512, &g);
    if (r == 0 && g == n * 512) return true;
    if (r < 0) botReset(); else scsiSense();
    delay(10);
  }
  return false;
}

static void usbRelease() {
  if (uXfer) { usb_host_transfer_free(uXfer); uXfer = nullptr; }
  if (uCtrl) { usb_host_transfer_free(uCtrl); uCtrl = nullptr; }
  if (uAttached) {
    usb_host_interface_release(uClient, uDev, uIf);
    usb_host_device_close(uClient, uDev);
  }
  uAttached = false;
  uReady = false;
  uGone = false;
}

// Ouvre le périphérique, trouve l'interface Mass Storage (classe 8 / SCSI / Bulk-Only) et le disque
static bool usbAttach(uint8_t addr) {
  if (usb_host_device_open(uClient, addr, &uDev) != ESP_OK) return false;
  const usb_config_desc_t *cfg = nullptr;
  if (usb_host_get_active_config_descriptor(uDev, &cfg) != ESP_OK || !cfg) { usb_host_device_close(uClient, uDev); return false; }
  const uint8_t *p = (const uint8_t *)cfg;
  uint32_t total = cfg->wTotalLength;
  bool inMsc = false;
  int intf = -1;
  uEpIn = uEpOut = 0;
  for (uint32_t i = 0; i + 2 <= total && p[i] != 0; i += p[i]) {
    if (p[i + 1] == 4) {                                            // interface
      inMsc = (p[i + 5] == 0x08 && p[i + 6] == 0x06 && p[i + 7] == 0x50 && p[i + 3] == 0);
      if (inMsc) { intf = p[i + 2]; uEpIn = uEpOut = 0; }
    } else if (p[i + 1] == 5 && inMsc && (p[i + 3] & 3) == 2) {     // endpoint bulk
      uint8_t ep = p[i + 2];
      uint16_t mps = p[i + 4] | (p[i + 5] << 8);
      if (ep & 0x80) { uEpIn = ep; uInMps = mps; } else { uEpOut = ep; uOutMps = mps; }
    }
  }
  if (intf < 0 || !uEpIn || !uEpOut) { usb_host_device_close(uClient, uDev); return false; }
  uIf = intf;
  if (usb_host_interface_claim(uClient, uDev, uIf, 0) != ESP_OK) { usb_host_device_close(uClient, uDev); return false; }
  uAttached = true;
  if (usb_host_transfer_alloc(XFER_MAX + 64, 0, &uXfer) != ESP_OK ||
      usb_host_transfer_alloc(64, 0, &uCtrl) != ESP_OK) { usbRelease(); return false; }

  // Attend que le disque soit prêt (TEST UNIT READY), puis vérifie la taille de bloc
  for (int i = 0; i < 40 && !uReady; i++) {
    uint8_t tur[6] = {0, 0, 0, 0, 0, 0};
    int r = botCmd(tur, 6, false, nullptr, 0, nullptr);
    if (r == 0) { uReady = true; break; }
    if (r < 0) botReset(); else scsiSense();
    delay(250);
  }
  if (!uReady) { Serial.println("USB : disque pas prêt"); usbRelease(); return false; }
  uint8_t cap[8], c[10] = {0x25};
  uint32_t g = 0;
  if (botCmd(c, 10, true, cap, 8, &g) != 0 || g < 8 || be32(cap + 4) != 512) {
    Serial.println("USB : taille de bloc non supportée (512 requis)");
    usbRelease();
    return false;
  }
  Serial.printf("USB : disque prêt (%lu secteurs)\n", (unsigned long)(be32(cap) + 1));
  return true;
}

static bool diskArrived() {
  static int attachTries = 0;
  if (!uAttached && uNew) {
    uNew = false;
    if (!usbAttach(uNewAddr)) {                 // périphérique non supporté ou pas prêt : quelques essais
      if (++attachTries < 5) { delay(1000); uNew = true; }
      return false;
    }
    attachTries = 0;
  }
  return uAttached && uReady;
}
static bool diskGone() { return uGone; }
static void diskRelease() { usbRelease(); }

#endif  // !HOST_TEST

// ============================ 4. FAT32 en lecture seule ============================
static uint32_t fatLba, dataLba, rootCluster, secPerClus;
static uint8_t fatCache[512];
static uint32_t fatCacheSec = 0xFFFFFFFF;

static bool isFat32Boot(const uint8_t *s) {
  return s[510] == 0x55 && s[511] == 0xAA && rd16(s + 11) == 512 && s[13] != 0 &&
         rd16(s + 22) == 0 && rd32(s + 36) != 0 && memcmp(s + 82, "FAT32", 5) == 0;
}

static bool fatMount() {
  uint8_t s[512];
  fatCacheSec = 0xFFFFFFFF;
  if (!usbReadSectors(0, 1, s)) return false;
  uint32_t base = 0;
  if (!isFat32Boot(s)) {                                   // MBR : première partition FAT32
    if (s[510] != 0x55 || s[511] != 0xAA) { Serial.println("FAT : secteur 0 invalide"); return false; }
    bool found = false;
    for (int i = 0; i < 4; i++) {
      const uint8_t *e = s + 446 + 16 * i;
      if (e[4] == 0x0B || e[4] == 0x0C) { base = rd32(e + 8); found = true; break; }
    }
    if (!found) { Serial.println("FAT : pas de partition FAT32 (exFAT/NTFS/GPT non supportés)"); return false; }
    if (!usbReadSectors(base, 1, s) || !isFat32Boot(s)) { Serial.println("FAT : secteur de boot invalide"); return false; }
  }
  secPerClus = s[13];
  uint32_t reserved = rd16(s + 14), nFats = s[16], fatSz = rd32(s + 36);
  rootCluster = rd32(s + 44);
  fatLba = base + reserved;
  dataLba = fatLba + nFats * fatSz;
  return true;
}

static inline uint32_t clusterLba(uint32_t c) { return dataLba + (c - 2) * secPerClus; }
static inline bool clusterOk(uint32_t c) { return c >= 2 && c < 0x0FFFFFF0; }

static uint32_t fatNext(uint32_t c) {
  uint32_t sec = fatLba + (c * 4) / 512;
  if (sec != fatCacheSec) {
    if (!usbReadSectors(sec, 1, fatCache)) return 0x0FFFFFFF;
    fatCacheSec = sec;
  }
  return rd32(fatCache + (c * 4) % 512) & 0x0FFFFFFF;
}

typedef bool (*DirCb)(const DirEnt &, void *);

// Parcourt un répertoire (entrées longues ignorées, on utilise les noms courts). true si le callback a stoppé.
static bool dirWalk(uint32_t cluster, DirCb cb, void *ctx) {
  uint8_t sec[512];
  uint32_t guard = 0;
  while (clusterOk(cluster) && guard++ < 100000) {
    for (uint32_t s = 0; s < secPerClus; s++) {
      if (!usbReadSectors(clusterLba(cluster) + s, 1, sec)) return false;
      for (int e = 0; e < 16; e++) {
        const uint8_t *p = sec + 32 * e;
        if (p[0] == 0x00) return false;                     // fin du répertoire
        if (p[0] == 0xE5 || p[0] == '.') continue;          // effacé, "." et ".."
        uint8_t attr = p[11];
        if ((attr & 0x0F) == 0x0F) continue;                // entrée de nom long
        if (attr & (0x08 | 0x02 | 0x04)) continue;          // volume, caché, système
        DirEnt d;
        int n = 0;
        for (int i = 0; i < 8 && p[i] != ' '; i++) d.name[n++] = p[i];
        if (p[8] != ' ') {
          d.name[n++] = '.';
          for (int i = 8; i < 11 && p[i] != ' '; i++) d.name[n++] = p[i];
        }
        d.name[n] = 0;
        d.attr = attr;
        d.cluster = (rd16(p + 20) << 16) | rd16(p + 26);
        d.size = rd32(p + 28);
        if (cb(d, ctx)) return true;
      }
    }
    cluster = fatNext(cluster);
  }
  return false;
}

static bool nameEq(const char *a, const char *b) {
  for (;; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x -= 32;
    if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return false;
    if (!x) return true;
  }
}

struct FindCtx { const char *name; DirEnt found; };
static bool cbFind(const DirEnt &e, void *c) {
  FindCtx *f = (FindCtx *)c;
  if (nameEq(e.name, f->name)) { f->found = e; return true; }
  return false;
}

// Résout "/dir/fichier.ext" (noms 8.3, insensible à la casse)
static bool fatFind(const char *path, DirEnt *out) {
  uint32_t cluster = rootCluster;
  char comp[16];
  while (*path == '/') path++;
  for (;;) {
    int n = 0;
    while (*path && *path != '/') { if (n < 15) comp[n++] = *path; path++; }
    comp[n] = 0;
    while (*path == '/') path++;
    FindCtx f;
    f.name = comp;
    if (!dirWalk(cluster, cbFind, &f)) return false;
    if (!*path) { *out = f.found; return true; }
    if (!(f.found.attr & 0x10)) return false;
    cluster = f.found.cluster;
  }
}

static bool isMp3(const DirEnt &e) {
  size_t n = strlen(e.name);
  return !(e.attr & 0x10) && e.size > 0 && n > 4 && nameEq(e.name + n - 4, ".MP3");
}

// --- lecture de fichier ---
static void fileOpen(FFile &f, const DirEnt &e, uint8_t *chunk) {
  f.first = e.cluster; f.size = e.size; f.pos = 0;
  f.curCl = e.cluster; f.curIdx = 0;
  f.chunk = chunk; f.chunkPos = 0; f.chunkLen = 0;
}

static bool fileFill(FFile &f) {
  uint32_t clBytes = secPerClus * 512;
  uint32_t idx = f.pos / clBytes;
  if (idx < f.curIdx) { f.curCl = f.first; f.curIdx = 0; }
  while (f.curIdx < idx) {
    f.curCl = fatNext(f.curCl);
    if (!clusterOk(f.curCl)) return false;
    f.curIdx++;
  }
  uint32_t secInCl = (f.pos % clBytes) / 512;
  uint32_t startOff = idx * clBytes + secInCl * 512;
  uint32_t left = (f.size - startOff + 511) / 512;
  uint32_t nsec = umin(umin(8, secPerClus - secInCl), left);
  if (nsec == 0 || !usbReadSectors(clusterLba(f.curCl) + secInCl, nsec, f.chunk)) return false;
  f.chunkPos = startOff;
  f.chunkLen = umin(nsec * 512, f.size - startOff);
  return true;
}

static int fileRead(FFile &f, uint8_t *buf, uint32_t n) {
  if (f.pos >= f.size) return 0;
  n = umin(n, f.size - f.pos);
  uint32_t total = 0;
  while (total < n) {
    if (f.pos >= f.chunkPos && f.pos < f.chunkPos + f.chunkLen) {
      uint32_t m = umin(n - total, f.chunkPos + f.chunkLen - f.pos);
      memcpy(buf + total, f.chunk + (f.pos - f.chunkPos), m);
      f.pos += m;
      total += m;
    } else if (!fileFill(f)) break;
  }
  return (int)total;
}

// ============================ 5. Lecteur ============================
static Voice bg, fg;
static bool mounted = false;
static uint32_t nextInsultAt = 0;
static uint32_t lastInsultCluster = 0;
static int emptyRetries = 0;

static bool voiceAlloc(Voice &v) {
  v.dec = malloc(sizeof(mp3dec_t));
  v.in = (uint8_t *)malloc(IN_BUF);
  v.fifo = (int16_t *)malloc(FIFO_N * sizeof(int16_t));
  v.file.chunk = (uint8_t *)malloc(4096);
  v.active = false;
  return v.dec && v.in && v.fifo && v.file.chunk;
}

static inline uint32_t fifoCount(const Voice &v) { return v.w - v.r; }
static inline uint32_t fifoFree(const Voice &v) { return FIFO_N - (v.w - v.r); }
static inline void fifoPush(Voice &v, int16_t s) { v.fifo[v.w & (FIFO_N - 1)] = s; v.w++; }
static inline int16_t fifoPop(Voice &v) { return v.fifo[v.r++ & (FIFO_N - 1)]; }

static void voiceRewind(Voice &v) {
  v.file.pos = 0;
  v.file.chunkLen = 0;
  v.inLen = 0;
  v.fileEof = false;
  v.ended = false;
  mp3dec_init((mp3dec_t *)v.dec);
}

static void voiceOpen(Voice &v, const DirEnt &e) {
  fileOpen(v.file, e, v.file.chunk);
  voiceRewind(v);
  v.r = v.w = v.acc = 0;
  v.frames = 0;
  v.active = true;
}

static void voiceStop(Voice &v) {
  v.active = false;
  v.r = v.w = 0;
}

// Décode des trames MP3 dans la FIFO (stéréo, rééchantillonné vers OUT_RATE)
static void voiceFeed(Voice &v) {
  static mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  int guard = 0;
  while (v.active && !v.ended && fifoFree(v) >= 8192 && guard++ < 3) {
    if (!v.fileEof && v.inLen < IN_BUF / 2) {
      int n = fileRead(v.file, v.in + v.inLen, IN_BUF - v.inLen);
      if (n <= 0) v.fileEof = true; else v.inLen += n;
    }
    if (v.inLen <= 0) { v.ended = true; return; }
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame((mp3dec_t *)v.dec, v.in, v.inLen, pcm, &info);
    if (info.frame_bytes == 0) {                    // aucune trame dans le tampon
      if (v.fileEof) { v.ended = true; return; }
      v.inLen = 0;
      continue;
    }
    if (samples > 0 && info.hz > 0) {
      int ch = info.channels;
      for (int i = 0; i < samples; i++) {
        int16_t l = pcm[i * ch];
        int16_t r = (ch > 1) ? pcm[i * ch + 1] : l;
        v.acc += OUT_RATE;
        while (v.acc >= (uint32_t)info.hz) {
          v.acc -= info.hz;
          fifoPush(v, l);
          fifoPush(v, r);
        }
      }
      v.frames++;
    }
    v.inLen -= info.frame_bytes;
    memmove(v.in, v.in + info.frame_bytes, v.inLen);
  }
}

// --- choix d'une insulte ---
struct PickCtx { int target, idx, count; DirEnt found; };
static bool cbCount(const DirEnt &e, void *c) { if (isMp3(e)) ((PickCtx *)c)->count++; return false; }
static bool cbPick(const DirEnt &e, void *c) {
  PickCtx *p = (PickCtx *)c;
  if (isMp3(e) && p->idx++ == p->target) { p->found = e; return true; }
  return false;
}

static bool pickInsult(DirEnt *out) {
  DirEnt dir;
  if (!fatFind(INSULT_DIR, &dir) || !(dir.attr & 0x10)) { Serial.println("Dossier /Insultes introuvable"); return false; }
  PickCtx p;
  p.count = 0; p.idx = 0; p.target = 0;
  dirWalk(dir.cluster, cbCount, &p);
  if (p.count == 0) { Serial.println("Aucun mp3 dans /Insultes"); return false; }
  for (int tries = 0; tries < 6; tries++) {
    p.idx = 0;
    p.target = random(p.count);
    if (!dirWalk(dir.cluster, cbPick, &p)) return false;
    if (p.count == 1 || p.found.cluster != lastInsultCluster) break;     // pas deux fois de suite la même
  }
  lastInsultCluster = p.found.cluster;
  *out = p.found;
  return true;
}

static void scheduleInsult() {
  nextInsultAt = millis() + (uint32_t)random((long)insultMinMs, (long)insultMaxMs + 1);
  Serial.printf("Prochaine insulte dans %lu s\n", (unsigned long)((nextInsultAt - millis()) / 1000));
}

static bool startBackground() {
  DirEnt e;
  if (!fatFind(BG_PATH, &e) || (e.attr & 0x10)) { Serial.printf("%s introuvable\n", BG_PATH); return false; }
  voiceOpen(bg, e);
  Serial.printf("Fond : %s\n", BG_PATH);
  return true;
}

static void unmountAll() {
  voiceStop(bg);
  voiceStop(fg);
  diskRelease();
  mounted = false;
  Serial.println("Clé USB retirée");
}

static void playerInit() {
  if (!voiceAlloc(bg) || !voiceAlloc(fg)) {
    Serial.println("Mémoire insuffisante");
    for (;;) delay(1000);
  }
}

static void audioWrite(const int16_t *d, uint32_t frames);     // I2S (carte) ou fichier (test PC)

static void audioMix() {
  static int16_t out[256 * 2];
  uint32_t frames = 256;
  if (bg.active) frames = umin(frames, fifoCount(bg) / 2);
  else if (fg.active) frames = umin(frames, fifoCount(fg) / 2);
  else { delay(5); return; }
  if (frames == 0) { delay(1); return; }
  for (uint32_t i = 0; i < frames * 2; i++) {
    int32_t s = 0;
    if (bg.active && fifoCount(bg) > 0) s += (int32_t)fifoPop(bg) * BG_VOL_PCT / 100;
    if (fg.active && fifoCount(fg) > 0) s += (int32_t)fifoPop(fg) * FG_VOL_PCT / 100;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    out[i] = (int16_t)s;
  }
  audioWrite(out, frames);
}

static void playerStep() {
  if (!mounted) {
    if (diskArrived()) {
      if (fatMount() && startBackground()) { mounted = true; scheduleInsult(); }
      else delay(2000);
    } else delay(50);
    return;
  }
  if (diskGone()) { unmountAll(); return; }

  if (bg.active && bg.ended) voiceRewind(bg);                     // boucle sans coupure (la FIFO contient la fin)

  if (!fg.active && (int32_t)(millis() - nextInsultAt) >= 0) {
    DirEnt e;
    if (pickInsult(&e)) {
      voiceOpen(fg, e);
      Serial.printf("Insulte : %s\n", e.name);
    } else scheduleInsult();
  }
  if (fg.active && fg.ended && fifoCount(fg) == 0) {              // insulte terminée
    bool empty = (fg.frames == 0);                                // fichier illisible (ex. ._xxx de macOS)
    voiceStop(fg);
    if (empty && ++emptyRetries < 5) nextInsultAt = millis();
    else { emptyRetries = 0; scheduleInsult(); }
  }

  voiceFeed(bg);
  voiceFeed(fg);
  audioMix();
}

// ============================ Arduino ============================
#ifndef HOST_TEST
static I2SClass i2s;

static void audioWrite(const int16_t *d, uint32_t frames) {
  i2s.write((uint8_t *)d, frames * 4);                      // bloquant : cadence la boucle
}

static void playerTask(void *) {
  for (;;) playerStep();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  randomSeed(esp_random());
  playerInit();

  i2s.setPins(PIN_BCLK, PIN_LRC, PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, OUT_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("Init I2S impossible");
    for (;;) delay(1000);
  }

  uSem = xSemaphoreCreateBinary();
  usb_host_config_t hcfg;
  memset(&hcfg, 0, sizeof(hcfg));
  hcfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&hcfg) != ESP_OK) { Serial.println("usb_host_install a échoué"); for (;;) delay(1000); }
  usb_host_client_config_t ccfg;
  memset(&ccfg, 0, sizeof(ccfg));
  ccfg.is_synchronous = false;
  ccfg.max_num_event_msg = 5;
  ccfg.async.client_event_callback = uClientCb;
  ccfg.async.callback_arg = nullptr;
  if (usb_host_client_register(&ccfg, &uClient) != ESP_OK) { Serial.println("client USB impossible"); for (;;) delay(1000); }
  xTaskCreatePinnedToCore(usbLibTask, "usb_lib", 4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(usbClientTask, "usb_cli", 4096, nullptr, 5, nullptr, 0);
  // minimp3 utilise ~17 Ko de pile : le décodage tourne dans une tâche dédiée
  xTaskCreatePinnedToCore(playerTask, "player", 32768, nullptr, 4, nullptr, 1);
  Serial.println("En attente d'une clé USB...");
}

void loop() {
  delay(1000);
}
#endif
