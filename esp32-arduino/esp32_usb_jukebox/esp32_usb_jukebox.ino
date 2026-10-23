/*
 * ESP32-S3 + MAX98357A + clé USB (host MSC)
 *   /fond/1.mp3      -> joué en boucle, volume 45 %
 *   /Insultes/*.mp3  -> un fichier au hasard toutes les 1 à 5 minutes, par-dessus le fond
 *
 * Carte Arduino : "ESP32S3 Dev Module", core esp32 >= 3.0
 *   USB CDC On Boot : Disabled   (le port USB natif sert de host, Serial = port UART)
 * Voir README.md pour les dépendances (libhelix + fichiers usb_host_msc).
 */
#include <Arduino.h>
#include <ESP_I2S.h>
#include <FS.h>
#include <vfs_api.h>
#include <vector>

#include "usb/usb_host.h"
#include "msc_host.h"
#include "msc_host_vfs.h"
#include "MP3DecoderHelix.h"

using namespace libhelix;

// ---------------- Réglages ----------------
static const int PIN_BCLK = 45;  // BLK
static const int PIN_LRC  = 48;  // LRC / WS
static const int PIN_DIN  = 47;  // DIN

static const uint32_t OUT_RATE = 44100;  // fréquence de sortie I2S (les autres sont rééchantillonnées)

static const char *BG_PATH     = "/fond/1.mp3";
static const char *INSULT_DIR  = "/Insultes";
static const int   BG_VOL_PCT  = 45;     // volume du fond
static const int   FG_VOL_PCT  = 100;    // volume des insultes

static const uint32_t INSULT_MIN_MS = 1UL * 60UL * 1000UL;
static const uint32_t INSULT_MAX_MS = 5UL * 60UL * 1000UL;

// ---------------- Système de fichiers USB ----------------
class USBFS : public fs::FS {
public:
  USBFS() : FS(fs::FSImplPtr(new VFSImpl())) {}
  void mountpoint(const char *mp) { _impl->mountpoint(mp); }
};
static USBFS usb;

static volatile bool usbConnected = false;
static volatile bool usbGone = false;
static volatile uint8_t usbAddr = 0;
static msc_host_device_handle_t mscDev = nullptr;
static msc_host_vfs_handle_t mscVfs = nullptr;
static bool mounted = false;

// ---------------- Voix (décodeur MP3 + FIFO PCM stéréo) ----------------
struct Voice {
  static const size_t N = 32768;  // échantillons int16 (puissance de 2)
  MP3DecoderHelix dec;
  File file;
  int16_t *buf = nullptr;
  size_t r = 0, w = 0;
  uint32_t acc = 0;
  bool active = false;
  bool eof = false;

  bool alloc() {
    buf = (int16_t *)ps_malloc(N * sizeof(int16_t));
    if (!buf) buf = (int16_t *)malloc(N * sizeof(int16_t));
    return buf != nullptr;
  }
  size_t count() const { return w - r; }
  size_t freeSpace() const { return N - count(); }
  void push(int16_t s) { if (count() < N) { buf[w & (N - 1)] = s; w++; } }
  int16_t pop() { int16_t s = buf[r & (N - 1)]; r++; return s; }
  void clear() { r = w = 0; acc = 0; }
};

static Voice bg, fg;

// Appelé par Helix à chaque frame décodée : met en stéréo + rééchantillonne vers OUT_RATE
static void onPcm(MP3FrameInfo &info, short *pcm, size_t len, void *ref) {
  Voice *v = (Voice *)ref;
  int ch = info.nChans > 0 ? info.nChans : 2;
  uint32_t srcRate = info.samprate > 0 ? info.samprate : OUT_RATE;
  size_t frames = len / ch;
  for (size_t i = 0; i < frames; i++) {
    int16_t l = pcm[i * ch];
    int16_t r = (ch > 1) ? pcm[i * ch + 1] : l;
    v->acc += OUT_RATE;
    while (v->acc >= srcRate) {
      v->acc -= srcRate;
      v->push(l);
      v->push(r);
    }
  }
}

static bool startVoice(Voice &v, const String &path) {
  v.file = usb.open(path.c_str(), "r");
  if (!v.file) {
    Serial.printf("Ouverture impossible : %s\n", path.c_str());
    return false;
  }
  v.clear();
  v.eof = false;
  v.dec.end();
  v.dec.begin();
  v.active = true;
  Serial.printf("Lecture : %s\n", path.c_str());
  return true;
}

static void stopVoice(Voice &v) {
  if (v.file) v.file.close();
  v.active = false;
  v.eof = false;
  v.clear();
}

static void refill(Voice &v) {
  if (!v.active || v.eof) return;
  uint8_t chunk[256];
  while (v.freeSpace() >= 16384) {
    int n = v.file.read(chunk, sizeof(chunk));
    if (n <= 0) { v.eof = true; break; }
    v.dec.write(chunk, n);
  }
}

// ---------------- Choix d'une insulte au hasard ----------------
static String pickInsult() {
  static String last;
  File d = usb.open(INSULT_DIR);
  if (!d || !d.isDirectory()) { Serial.println("Dossier /Insultes introuvable"); return ""; }
  std::vector<String> names;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String n = f.name();
    String l = n;
    l.toLowerCase();
    if (f.isDirectory() || n.startsWith(".") || !l.endsWith(".mp3")) continue;  // ignore les ._xxx de macOS
    names.push_back(n);
  }
  d.close();
  if (names.empty()) { Serial.println("Aucun mp3 dans /Insultes"); return ""; }
  String pick = names[random(names.size())];
  if (names.size() > 1) {
    while (pick == last) pick = names[random(names.size())];  // pas deux fois de suite le même
  }
  last = pick;
  return String(INSULT_DIR) + "/" + pick;
}

// ---------------- USB host ----------------
static void mscEvent(const msc_host_event_t *ev, void *arg) {
  if (ev->event == MSC_DEVICE_CONNECTED) {
    usbAddr = ev->device.address;
    usbConnected = true;
  } else if (ev->event == MSC_DEVICE_DISCONNECTED) {
    usbGone = true;
  }
}

static void usbLibTask(void *) {
  while (true) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

static bool mountUsb() {
  if (msc_host_install_device(usbAddr, &mscDev) != ESP_OK) return false;
  esp_vfs_fat_mount_config_t mc = {};
  mc.format_if_mount_failed = false;
  mc.max_files = 5;
  mc.allocation_unit_size = 8192;
  if (msc_host_vfs_register(mscDev, "/usb", &mc, &mscVfs) != ESP_OK) {
    msc_host_uninstall_device(mscDev);
    mscDev = nullptr;
    return false;
  }
  usb.mountpoint("/usb");
  mounted = true;
  Serial.println("Clé USB montée");
  return true;
}

static void unmountUsb() {
  stopVoice(bg);
  stopVoice(fg);
  if (mscVfs) msc_host_vfs_unregister(mscVfs);
  if (mscDev) msc_host_uninstall_device(mscDev);
  mscVfs = nullptr;
  mscDev = nullptr;
  mounted = false;
  usbGone = false;
  usbConnected = false;
  Serial.println("Clé USB retirée");
}

// ---------------- I2S ----------------
static I2SClass i2s;

// ---------------- Programme ----------------
static uint32_t nextInsultAt = 0;

static void scheduleInsult() {
  nextInsultAt = millis() + random(INSULT_MIN_MS, INSULT_MAX_MS + 1);
  Serial.printf("Prochaine insulte dans %lu s\n", (nextInsultAt - millis()) / 1000);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  randomSeed(esp_random());

  if (!bg.alloc() || !fg.alloc()) {
    Serial.println("Mémoire insuffisante");
    while (true) delay(1000);
  }
  bg.dec.setDataCallback(onPcm);
  bg.dec.setReference(&bg);
  fg.dec.setDataCallback(onPcm);
  fg.dec.setReference(&fg);

  i2s.setPins(PIN_BCLK, PIN_LRC, PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, OUT_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("Init I2S impossible");
    while (true) delay(1000);
  }

  usb_host_config_t hcfg = {};
  hcfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  ESP_ERROR_CHECK(usb_host_install(&hcfg));
  xTaskCreatePinnedToCore(usbLibTask, "usb_lib", 4096, nullptr, 5, nullptr, 0);

  msc_host_driver_config_t mcfg = {};
  mcfg.create_backround_task = true;  // (sic) orthographe de la lib
  mcfg.task_priority = 5;
  mcfg.stack_size = 4096;
  mcfg.callback = mscEvent;
  ESP_ERROR_CHECK(msc_host_install(&mcfg));

  Serial.println("En attente d'une clé USB...");
}

void loop() {
  if (!mounted) {
    if (usbConnected && mountUsb()) {
      if (startVoice(bg, BG_PATH)) scheduleInsult();
    }
    delay(50);
    return;
  }
  if (usbGone) { unmountUsb(); return; }

  // Fond en boucle (la FIFO contient encore la fin du morceau : pas de coupure)
  if (bg.active && bg.eof) {
    bg.file.seek(0);
    bg.eof = false;
    bg.dec.end();
    bg.dec.begin();
  }

  // Déclenchement d'une insulte
  if (!fg.active && (int32_t)(millis() - nextInsultAt) >= 0) {
    String p = pickInsult();
    if (p.length() == 0 || !startVoice(fg, p)) scheduleInsult();
  }
  // Fin d'une insulte (fichier lu + FIFO vidée)
  if (fg.active && fg.eof && fg.count() == 0) {
    stopVoice(fg);
    scheduleInsult();
  }

  refill(bg);
  refill(fg);

  // Mixage : 256 trames stéréo par passage ; i2s.write() bloque tant que le DMA est plein
  static int16_t out[256 * 2];
  size_t frames = 256;
  if (bg.active) frames = min(frames, bg.count() / 2);
  else if (fg.active) frames = min(frames, fg.count() / 2);
  else { delay(5); return; }
  if (frames == 0) { delay(1); return; }

  for (size_t i = 0; i < frames * 2; i++) {
    int32_t s = 0;
    if (bg.active && bg.count() > 0) s += (int32_t)bg.pop() * BG_VOL_PCT / 100;
    if (fg.active && fg.count() > 0) s += (int32_t)fg.pop() * FG_VOL_PCT / 100;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    out[i] = (int16_t)s;
  }
  i2s.write((uint8_t *)out, frames * 2 * sizeof(int16_t));
}
