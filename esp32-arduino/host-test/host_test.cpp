// Test PC du sketch : le "disque USB" est un fichier image FAT32, la sortie I2S un fichier PCM brut.
// usage : host_test image.img out.pcm [secondes_virtuelles] [delai_insulte_ms]
#define HOST_TEST
#include <Arduino.h>

uint32_t g_millis = 0;
static FILE *img, *pcmOut;
static uint64_t framesOut = 0;
static uint32_t sectorReads = 0;

static bool usbReadSectors(uint32_t lba, uint32_t n, uint8_t *buf) {
  sectorReads++;
  if (fseek(img, (long)lba * 512, SEEK_SET) != 0) return false;
  return fread(buf, 512, n, img) == n;
}
static bool diskArrived() { return true; }
static bool diskGone() { return false; }
static void diskRelease() {}
static void audioWrite(const int16_t *d, uint32_t frames) {
  fwrite(d, 4, frames, pcmOut);
  framesOut += frames;
  g_millis = (uint32_t)(framesOut * 1000ULL / 44100ULL);      // le temps avance au rythme de l'audio
}

#include "../esp32_usb_jukebox/esp32_usb_jukebox.ino"

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage\n"); return 2; }
  img = fopen(argv[1], "rb");
  pcmOut = fopen(argv[2], "wb");
  double seconds = argc > 3 ? atof(argv[3]) : 30;
  if (!img || !pcmOut) return 2;
  insultMinMs = 3000;                                          // accéléré pour le test
  insultMaxMs = 6000;
  if (argc > 4) insultMinMs = insultMaxMs = (uint32_t)atoi(argv[4]);   // 4e argument : délai fixe (ms)
  srand(1);
  playerInit();
  while (framesOut < seconds * 44100) playerStep();
  fprintf(stderr, "ok : %.1f s audio, %u lectures de secteurs\n", (double)framesOut / 44100, sectorReads);
  return 0;
}
