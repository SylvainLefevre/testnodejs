// Test PC du sketch : la "clé USB" est un dossier du PC, la sortie I2S un fichier PCM brut.
// usage : host_test dossier_cle out.pcm [secondes_virtuelles] [delai_insulte_ms]
#define HOST_TEST
#include <Arduino.h>
#include <FS.h>

uint32_t g_millis = 0;
static FILE *pcmOut;
static fs::FS hostFs;
static uint64_t framesOut = 0;

static fs::FS &diskFs() { return hostFs; }
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
  if (argc < 3) { fprintf(stderr, "usage: host_test dossier out.pcm [secondes] [delai_insulte_ms]\n"); return 2; }
  hostFs = fs::FS(argv[1]);
  pcmOut = fopen(argv[2], "wb");
  double seconds = argc > 3 ? atof(argv[3]) : 30;
  if (!pcmOut) return 2;
  insultMinMs = 3000;                                          // accéléré pour le test
  insultMaxMs = 6000;
  if (argc > 4) insultMinMs = insultMaxMs = (uint32_t)atoi(argv[4]);
  srand(1);
  playerInit();
  while (framesOut < seconds * 44100) playerStep();
  fprintf(stderr, "ok : %.1f s audio\n", (double)framesOut / 44100);
  return 0;
}
