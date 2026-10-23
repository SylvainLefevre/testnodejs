// ESP32-S3 + MAX98357A + clé USB (port USB OTG)
//  - /usb/fond/1.mp3      : fond sonore en boucle, volume 45 %
//  - /usb/Insultes/*.mp3  : un fichier au hasard toutes les 1 à 5 minutes, par-dessus le fond
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mp3dec.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"

static const char *TAG = "player";

// ---- Brochage MAX98357A ----
#define I2S_LRC_GPIO 48  // LRC  -> LRCLK / WS
#define I2S_BCLK_GPIO 45 // BLK  -> BCLK
#define I2S_DIN_GPIO 47  // DIN  -> données

// ---- Audio ----
#define OUT_RATE 44100
#define BLOCK_FRAMES 256
#define BG_VOLUME_PCT 45      // volume du fond sonore
#define INSULT_VOLUME_PCT 100 // volume des insultes
#define INSULT_MIN_S 60
#define INSULT_MAX_S 300

#define MOUNT_POINT "/usb"
#define BG_FILE MOUNT_POINT "/fond/1.mp3"
#define INSULT_DIR MOUNT_POINT "/Insultes"
#define MAX_INSULTS 512

#define IN_BUF_SIZE 4096

typedef struct {
    FILE *f;
    HMP3Decoder dec;
    uint8_t in[IN_BUF_SIZE];
    uint8_t *ptr;
    int left;
    bool eof;
    int16_t pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
    int frames; // trames PCM décodées disponibles
    int chans;
    int pos;      // trame courante
    uint32_t acc; // phase du rééchantillonneur (16.16)
    uint32_t step;
} src_t;

static src_t bg, ins;
static bool ins_active;
static char *insult_files[MAX_INSULTS];
static int insult_count;

static i2s_chan_handle_t tx;
static EventGroupHandle_t usb_ev;
#define EV_CONNECTED BIT0
#define EV_DISCONNECTED BIT1
static uint8_t usb_addr;
static msc_host_device_handle_t usb_dev_handle;

// ---------------------------------------------------------------- décodeur MP3

static void src_close(src_t *s)
{
    if (s->f) fclose(s->f);
    if (s->dec) MP3FreeDecoder(s->dec);
    s->f = NULL;
    s->dec = NULL;
}

static bool src_open(src_t *s, const char *path)
{
    src_close(s);
    s->f = fopen(path, "rb");
    if (!s->f) {
        ESP_LOGE(TAG, "ouverture impossible: %s", path);
        return false;
    }
    s->dec = MP3InitDecoder();
    if (!s->dec) {
        src_close(s);
        return false;
    }
    // saute l'en-tête ID3v2 éventuel
    uint8_t h[10];
    if (fread(h, 1, 10, s->f) == 10 && !memcmp(h, "ID3", 3)) {
        long sz = ((h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
        fseek(s->f, 10 + sz, SEEK_SET);
    } else {
        fseek(s->f, 0, SEEK_SET);
    }
    s->ptr = s->in;
    s->left = 0;
    s->eof = false;
    s->frames = s->pos = 0;
    s->acc = 0;
    s->step = 65536;
    s->chans = 2;
    ESP_LOGI(TAG, "lecture: %s", path);
    return true;
}

// Décode une trame MP3 dans s->pcm. false = fin de fichier / erreur fatale.
static bool decode_next(src_t *s)
{
    for (;;) {
        if (s->left < 2048 && !s->eof) {
            memmove(s->in, s->ptr, s->left);
            size_t n = fread(s->in + s->left, 1, IN_BUF_SIZE - s->left, s->f);
            if (n == 0) s->eof = true;
            s->ptr = s->in;
            s->left += n;
        }
        if (s->left <= 0) return false;

        int off = MP3FindSyncWord(s->ptr, s->left);
        if (off < 0) {
            s->left = 0; // pas de synchro dans le tampon, on le jette
            continue;
        }
        s->ptr += off;
        s->left -= off;

        int err = MP3Decode(s->dec, &s->ptr, &s->left, s->pcm, 0);
        if (err == ERR_MP3_NONE) {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(s->dec, &fi);
            if (fi.samprate <= 0 || fi.nChans <= 0) continue;
            s->chans = fi.nChans;
            s->frames = fi.outputSamps / fi.nChans;
            s->step = ((uint64_t)fi.samprate << 16) / OUT_RATE;
            return true;
        }
        if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (s->eof) return false;
            continue; // il faut plus de données
        }
        // trame corrompue: on avance d'un octet
        if (s->left > 0) {
            s->ptr++;
            s->left--;
        }
    }
}

// Renvoie la prochaine trame stéréo à OUT_RATE (rééchantillonnage par maintien d'ordre 0).
static bool src_next(src_t *s, int *l, int *r)
{
    while (s->pos >= s->frames) {
        s->pos -= s->frames;
        if (!decode_next(s)) return false;
    }
    if (s->chans == 1) {
        *l = *r = s->pcm[s->pos];
    } else {
        *l = s->pcm[2 * s->pos];
        *r = s->pcm[2 * s->pos + 1];
    }
    s->acc += s->step;
    s->pos += s->acc >> 16;
    s->acc &= 0xFFFF;
    return true;
}

// ---------------------------------------------------------------- I2S

static void i2s_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &tx, NULL));

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(OUT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_LRC_GPIO,
            .dout = I2S_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
}

// ---------------------------------------------------------------- USB

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    if (event->event == MSC_DEVICE_CONNECTED) {
        usb_addr = event->device.address;
        xEventGroupSetBits(usb_ev, EV_CONNECTED);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        xEventGroupSetBits(usb_ev, EV_DISCONNECTED);
    }
}

static void usb_init(void)
{
    usb_ev = xEventGroupCreate();
    const usb_host_config_t host_cfg = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 5, NULL);

    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_cfg));
}

// ---------------------------------------------------------------- liste des insultes

static bool has_mp3_ext(const char *n)
{
    size_t len = strlen(n);
    return len > 4 && !strcasecmp(n + len - 4, ".mp3") && n[0] != '.';
}

static void scan_insults(void)
{
    for (int i = 0; i < insult_count; i++) free(insult_files[i]);
    insult_count = 0;

    DIR *d = opendir(INSULT_DIR);
    if (!d) {
        ESP_LOGE(TAG, "répertoire introuvable: %s", INSULT_DIR);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) && insult_count < MAX_INSULTS) {
        if (!has_mp3_ext(e->d_name)) continue;
        size_t len = strlen(INSULT_DIR) + 1 + strlen(e->d_name) + 1;
        char *p = malloc(len);
        if (!p) break;
        snprintf(p, len, "%s/%s", INSULT_DIR, e->d_name);
        insult_files[insult_count++] = p;
    }
    closedir(d);
    ESP_LOGI(TAG, "%d fichier(s) dans %s", insult_count, INSULT_DIR);
}

static int64_t next_insult_us(void)
{
    uint32_t s = INSULT_MIN_S + esp_random() % (INSULT_MAX_S - INSULT_MIN_S + 1);
    ESP_LOGI(TAG, "prochaine insulte dans %u s", (unsigned)s);
    return esp_timer_get_time() + (int64_t)s * 1000000;
}

// ---------------------------------------------------------------- lecture

static inline int16_t clip16(int v)
{
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

static void play_session(void)
{
    scan_insults();
    bool bg_ok = src_open(&bg, BG_FILE);
    ins_active = false;
    int64_t next_us = next_insult_us();

    static int16_t out[BLOCK_FRAMES * 2];
    const int bg_gain = BG_VOLUME_PCT * 256 / 100;
    const int ins_gain = INSULT_VOLUME_PCT * 256 / 100;

    while (!(xEventGroupGetBits(usb_ev) & EV_DISCONNECTED)) {
        if (!ins_active && insult_count > 0 && esp_timer_get_time() >= next_us) {
            ins_active = src_open(&ins, insult_files[esp_random() % insult_count]);
            if (!ins_active) next_us = next_insult_us();
        }

        for (int i = 0; i < BLOCK_FRAMES; i++) {
            int l = 0, r = 0, a, b;

            if (bg_ok) {
                if (!src_next(&bg, &a, &b)) {         // fin du fond: on reboucle
                    bg_ok = src_open(&bg, BG_FILE) && src_next(&bg, &a, &b);
                }
                if (bg_ok) {
                    l += a * bg_gain >> 8;
                    r += b * bg_gain >> 8;
                }
            }
            if (ins_active) {
                if (src_next(&ins, &a, &b)) {
                    l += a * ins_gain >> 8;
                    r += b * ins_gain >> 8;
                } else {
                    src_close(&ins);
                    ins_active = false;
                    next_us = next_insult_us();
                }
            }
            out[2 * i] = clip16(l);
            out[2 * i + 1] = clip16(r);
        }

        size_t written;
        i2s_channel_write(tx, out, sizeof(out), &written, portMAX_DELAY);
        if (!bg_ok && !ins_active) vTaskDelay(1); // rien à jouer: évite de saturer le CPU
    }

    src_close(&bg);
    src_close(&ins);
    ins_active = false;
}

void app_main(void)
{
    i2s_init();
    usb_init();

    for (;;) {
        ESP_LOGI(TAG, "en attente d'une clé USB...");
        xEventGroupClearBits(usb_ev, EV_DISCONNECTED);
        xEventGroupWaitBits(usb_ev, EV_CONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);

        msc_host_vfs_handle_t vfs = NULL;
        if (msc_host_install_device(usb_addr, &usb_dev_handle) != ESP_OK) continue;

        const esp_vfs_fat_mount_config_t mnt = {
            .format_if_mount_failed = false,
            .max_files = 4,
            .allocation_unit_size = 8192,
        };
        if (msc_host_vfs_register(usb_dev_handle, MOUNT_POINT, &mnt, &vfs) == ESP_OK) {
            ESP_LOGI(TAG, "clé USB montée sur " MOUNT_POINT);
            play_session();
            msc_host_vfs_unregister(vfs);
        } else {
            ESP_LOGE(TAG, "montage FAT impossible (clé en FAT32 ?)");
            xEventGroupWaitBits(usb_ev, EV_DISCONNECTED, pdFALSE, pdFALSE, portMAX_DELAY);
        }
        msc_host_uninstall_device(usb_dev_handle);
    }
}
