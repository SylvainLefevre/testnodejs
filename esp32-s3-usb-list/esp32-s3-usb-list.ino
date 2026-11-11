// Liste dans le moniteur série tous les fichiers d'une clé USB branchée sur le
// port USB OTG de l'ESP32-S3-DevKitC-1 v1.1.
//
// Nécessite le core Arduino-ESP32 3.x. Aucune bibliothèque à installer : le sketch
// utilise la pile hôte USB et FatFs déjà présentes dans le core, et implémente
// lui-même le protocole "USB Mass Storage" (Bulk-Only + commandes SCSI).
// La clé est lue en lecture seule : rien n'est jamais écrit dessus.

#include <Arduino.h>
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "ff.h"
#include "diskio.h"
#include "diskio_impl.h"

static const uint32_t XFER_TIMEOUT_MS = 5000;
static const size_t   DATA_BUF_SIZE   = 4096;  // lecture par blocs de 4 Ko max

// ---------------------------------------------------------------------------
// État global
// ---------------------------------------------------------------------------
struct UsbEvent {
  bool connected;
  uint8_t address;
};

static QueueHandle_t eventQueue;
static SemaphoreHandle_t xferDone;
static usb_host_client_handle_t client = nullptr;

static usb_device_handle_t dev = nullptr;
static uint8_t ifaceNum, epIn, epOut;
static uint16_t mpsIn = 64;
static bool deviceFailed = false;

static usb_transfer_t *xferOut, *xferIn, *xferCtrl;
static uint32_t cbwTag = 1;
static uint32_t blockCount = 0, blockSize = 512;

static BYTE pdrv = 0xFF;
static FATFS fatfs;
static char drivePath[4];

// ---------------------------------------------------------------------------
// Tâches de la pile hôte USB
// ---------------------------------------------------------------------------
static void usbLibTask(void *) {
  while (true) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

static void usbClientTask(void *) {
  while (true) usb_host_client_handle_events(client, portMAX_DELAY);
}

static void clientEventCb(const usb_host_client_event_msg_t *msg, void *) {
  UsbEvent ev = {};
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    ev.connected = true;
    ev.address = msg->new_dev.address;
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    ev.connected = false;
  } else {
    return;
  }
  xQueueSend(eventQueue, &ev, 0);
}

// ---------------------------------------------------------------------------
// Transferts USB (bloquants)
// ---------------------------------------------------------------------------
static void xferCb(usb_transfer_t *t) {
  xSemaphoreGive((SemaphoreHandle_t)t->context);
}

static bool runTransfer(usb_transfer_t *t, bool control = false) {
  if (deviceFailed) return false;
  xSemaphoreTake(xferDone, 0);  // purge un éventuel signal en retard
  t->device_handle = dev;
  t->callback = xferCb;
  t->context = xferDone;
  esp_err_t err = control ? usb_host_transfer_submit_control(client, t) : usb_host_transfer_submit(t);
  if (err != ESP_OK) return false;
  if (xSemaphoreTake(xferDone, pdMS_TO_TICKS(XFER_TIMEOUT_MS)) != pdTRUE) {
    Serial.println("Délai dépassé sur un transfert USB");
    deviceFailed = true;
    return false;
  }
  if (t->status == USB_TRANSFER_STATUS_NO_DEVICE) deviceFailed = true;
  return t->status == USB_TRANSFER_STATUS_COMPLETED;
}

// Débloque un endpoint qui a répondu STALL (côté hôte puis côté clé).
static void clearStall(uint8_t ep) {
  usb_host_endpoint_halt(dev, ep);
  usb_host_endpoint_flush(dev, ep);
  usb_host_endpoint_clear(dev, ep);
  uint8_t *s = xferCtrl->data_buffer;  // requête CLEAR_FEATURE(ENDPOINT_HALT)
  s[0] = 0x02; s[1] = 0x01; s[2] = 0; s[3] = 0;
  s[4] = ep;   s[5] = 0;    s[6] = 0; s[7] = 0;
  xferCtrl->num_bytes = 8;
  xferCtrl->bEndpointAddress = 0;
  runTransfer(xferCtrl, true);
}

// ---------------------------------------------------------------------------
// Protocole Mass Storage "Bulk-Only Transport" (données de la clé vers l'ESP32)
// ---------------------------------------------------------------------------
static void put32le(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint32_t get32le(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t get32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
static size_t roundUp(size_t n, size_t mps) {
  return (n + mps - 1) / mps * mps;
}

static bool scsiCommand(const uint8_t *cb, uint8_t cbLen, uint8_t *data, uint32_t len) {
  // 1. Command Block Wrapper
  uint32_t tag = cbwTag++;
  uint8_t *c = xferOut->data_buffer;
  memset(c, 0, 31);
  put32le(c, 0x43425355);  // "USBC"
  put32le(c + 4, tag);
  put32le(c + 8, len);
  c[12] = len ? 0x80 : 0x00;  // sens : clé -> hôte
  c[13] = 0;                  // LUN 0
  c[14] = cbLen;
  memcpy(c + 15, cb, cbLen);
  xferOut->num_bytes = 31;
  xferOut->bEndpointAddress = epOut;
  if (!runTransfer(xferOut)) {
    if (xferOut->status == USB_TRANSFER_STATUS_STALL) clearStall(epOut);
    return false;
  }

  // 2. Données
  if (len) {
    xferIn->num_bytes = roundUp(len, mpsIn);
    xferIn->bEndpointAddress = epIn;
    if (runTransfer(xferIn)) {
      memcpy(data, xferIn->data_buffer, min((uint32_t)xferIn->actual_num_bytes, len));
    } else if (xferIn->status == USB_TRANSFER_STATUS_STALL) {
      clearStall(epIn);  // on lit quand même le CSW
    } else {
      return false;
    }
  }

  // 3. Command Status Wrapper
  for (int attempt = 0; attempt < 2; attempt++) {
    xferIn->num_bytes = roundUp(13, mpsIn);
    xferIn->bEndpointAddress = epIn;
    if (runTransfer(xferIn)) break;
    if (attempt == 0 && xferIn->status == USB_TRANSFER_STATUS_STALL) {
      clearStall(epIn);
      continue;
    }
    return false;
  }
  const uint8_t *s = xferIn->data_buffer;
  if (xferIn->actual_num_bytes < 13 || get32le(s) != 0x53425355 /* "USBS" */ || get32le(s + 4) != tag) {
    return false;
  }
  return s[12] == 0;  // 0 = commande réussie
}

static bool scsiInquiry(char *vendor, char *product) {
  const uint8_t cb[6] = {0x12, 0, 0, 0, 36, 0};
  uint8_t r[36] = {};
  if (!scsiCommand(cb, 6, r, 36)) return false;
  memcpy(vendor, r + 8, 8);    vendor[8] = '\0';
  memcpy(product, r + 16, 16); product[16] = '\0';
  return true;
}

static bool scsiTestUnitReady() {
  const uint8_t cb[6] = {0x00, 0, 0, 0, 0, 0};
  return scsiCommand(cb, 6, nullptr, 0);
}

static void scsiRequestSense() {
  const uint8_t cb[6] = {0x03, 0, 0, 0, 18, 0};
  uint8_t r[18];
  scsiCommand(cb, 6, r, 18);
}

static bool scsiReadCapacity() {
  const uint8_t cb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t r[8] = {};
  if (!scsiCommand(cb, 10, r, 8)) return false;
  uint32_t lastLba = get32be(r);
  blockSize = get32be(r + 4);
  if (lastLba == 0xFFFFFFFF) return false;  // > 2 To : non géré
  blockCount = lastLba + 1;
  return true;
}

static bool scsiRead(uint32_t lba, uint16_t count, uint8_t *dst) {
  const uint8_t cb[10] = {0x28, 0,
                          (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                          0, (uint8_t)(count >> 8), (uint8_t)count, 0};
  return scsiCommand(cb, 10, dst, (uint32_t)count * blockSize);
}

// ---------------------------------------------------------------------------
// Pilote disque pour FatFs (lecture seule)
// ---------------------------------------------------------------------------
static DSTATUS diskInit(unsigned char) {
  return (dev && !deviceFailed) ? 0 : STA_NOINIT;
}

static DSTATUS diskStatus(unsigned char) {
  return (dev && !deviceFailed) ? STA_PROTECT : STA_NOINIT;
}

static DRESULT diskRead(unsigned char, unsigned char *buff, uint32_t sector, unsigned count) {
  const uint32_t maxBlocks = DATA_BUF_SIZE / blockSize;
  while (count) {
    uint32_t n = min((uint32_t)count, maxBlocks);
    if (!scsiRead(sector, n, buff)) return RES_ERROR;
    buff += n * blockSize;
    sector += n;
    count -= n;
  }
  return RES_OK;
}

static DRESULT diskWrite(unsigned char, const unsigned char *, uint32_t, unsigned) {
  return RES_WRPRT;  // on n'écrit jamais sur la clé
}

static DRESULT diskIoctl(unsigned char, unsigned char cmd, void *buff) {
  switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *(DWORD *)buff = blockCount; return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD *)buff = blockSize;   return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 1;          return RES_OK;
  }
  return RES_PARERR;
}

static const ff_diskio_impl_t usbDiskImpl = {
  .init = &diskInit,
  .status = &diskStatus,
  .read = &diskRead,
  .write = &diskWrite,
  .ioctl = &diskIoctl,
};

// ---------------------------------------------------------------------------
// Listage récursif des fichiers
// ---------------------------------------------------------------------------
static char path[512];
static FILINFO fno;  // partagé : on n'en a plus besoin après la descente récursive
static uint32_t fileCount, dirCount;
static uint64_t totalBytes;

static void indent(int depth) {
  for (int i = 0; i < depth; i++) Serial.print("  ");
}

static void listDir(size_t len, int depth) {
  FF_DIR dir;
  if (f_opendir(&dir, path) != FR_OK) {
    indent(depth);
    Serial.printf("[impossible d'ouvrir %s]\n", path);
    return;
  }
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
    indent(depth);
    if (fno.fattrib & AM_DIR) {
      Serial.printf("%s/\n", fno.fname);
      dirCount++;
      int n = snprintf(path + len, sizeof(path) - len, "/%s", fno.fname);
      if (n > 0 && (size_t)n < sizeof(path) - len) {
        listDir(len + n, depth + 1);
      }
      path[len] = '\0';
    } else {
      Serial.print(fno.fname);
      Serial.print("  (");
      Serial.print((unsigned long long)fno.fsize);
      Serial.println(" octets)");
      fileCount++;
      totalBytes += fno.fsize;
    }
  }
  f_closedir(&dir);
}

// ---------------------------------------------------------------------------
// Gestion d'une clé
// ---------------------------------------------------------------------------
// Cherche l'interface Mass Storage (classe 08, SCSI 06, Bulk-Only 50) et ses endpoints.
static bool findMscInterface() {
  const usb_config_desc_t *cfg;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return false;
  const uint8_t *p = (const uint8_t *)cfg;
  const uint16_t total = cfg->wTotalLength;
  bool inMsc = false, found = false;
  epIn = epOut = 0;

  for (uint16_t off = 0; off + 2 <= total && p[off] != 0; off += p[off]) {
    const uint8_t type = p[off + 1];
    if (type == 0x04) {  // interface
      if (found) break;
      inMsc = p[off + 3] == 0 && p[off + 5] == 0x08 && p[off + 6] == 0x06 && p[off + 7] == 0x50;
      if (inMsc) { ifaceNum = p[off + 2]; found = true; }
    } else if (type == 0x05 && inMsc && (p[off + 3] & 0x03) == 0x02) {  // endpoint bulk
      const uint8_t addr = p[off + 2];
      if (addr & 0x80) { epIn = addr; mpsIn = p[off + 4] | (p[off + 5] << 8); }
      else             { epOut = addr; }
    }
  }
  return found && epIn && epOut;
}

static void closeDevice() {
  if (!dev) return;
  usb_host_interface_release(client, dev, ifaceNum);
  usb_host_device_close(client, dev);
  dev = nullptr;
}

static void handleNewDevice(uint8_t address) {
  if (dev) return;  // une seule clé à la fois
  deviceFailed = false;

  if (usb_host_device_open(client, address, &dev) != ESP_OK) {
    dev = nullptr;
    return;
  }
  if (!findMscInterface()) {
    Serial.println("Périphérique USB détecté, mais ce n'est pas une clé USB.");
    usb_host_device_close(client, dev);
    dev = nullptr;
    return;
  }
  if (usb_host_interface_claim(client, dev, ifaceNum, 0) != ESP_OK) {
    Serial.println("Impossible de réserver l'interface de la clé.");
    usb_host_device_close(client, dev);
    dev = nullptr;
    return;
  }

  char vendor[9], product[17];
  if (scsiInquiry(vendor, product)) {
    Serial.printf("Clé détectée : %s %s\n", vendor, product);
  }

  bool ready = false;
  for (int i = 0; i < 10 && !deviceFailed; i++) {
    if (scsiTestUnitReady()) { ready = true; break; }
    scsiRequestSense();
    delay(200);
  }
  if (!ready || !scsiReadCapacity() || blockSize < 512 || blockSize > FF_MAX_SS) {
    Serial.println("La clé ne répond pas correctement.");
    return;  // la clé reste ouverte jusqu'à son retrait
  }
  Serial.printf("Capacité : %lu Mo (%lu secteurs de %lu octets)\n",
                (unsigned long)((uint64_t)blockCount * blockSize / (1024 * 1024)),
                (unsigned long)blockCount, (unsigned long)blockSize);

  if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) {
    Serial.println("Plus de lecteur FatFs disponible.");
    return;
  }
  ff_diskio_register(pdrv, &usbDiskImpl);
  snprintf(drivePath, sizeof(drivePath), "%u:", pdrv);

  FRESULT res = f_mount(&fatfs, drivePath, 1);
  if (res != FR_OK) {
    Serial.printf("Montage impossible (erreur FatFs %d). La clé est-elle en FAT32 ?\n", res);
  } else {
    fileCount = dirCount = 0;
    totalBytes = 0;
    strcpy(path, drivePath);

    Serial.println("\n===== Contenu de la clé USB =====");
    listDir(strlen(path), 0);
    Serial.printf("===== %lu fichier(s), %lu dossier(s), ",
                  (unsigned long)fileCount, (unsigned long)dirCount);
    Serial.print((unsigned long long)totalBytes);
    Serial.println(" octets =====\n");

    f_mount(nullptr, drivePath, 0);  // démontage
  }
  ff_diskio_register(pdrv, nullptr);
  pdrv = 0xFF;
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  eventQueue = xQueueCreate(5, sizeof(UsbEvent));
  xferDone = xSemaphoreCreateBinary();

  usb_host_config_t hostConfig = {};
  hostConfig.skip_phy_setup = false;
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;
  ESP_ERROR_CHECK(usb_host_install(&hostConfig));
  xTaskCreate(usbLibTask, "usb_lib", 4096, nullptr, 2, nullptr);

  usb_host_client_config_t clientConfig = {};
  clientConfig.is_synchronous = false;
  clientConfig.max_num_event_msg = 5;
  clientConfig.async.client_event_callback = clientEventCb;
  clientConfig.async.callback_arg = nullptr;
  ESP_ERROR_CHECK(usb_host_client_register(&clientConfig, &client));
  xTaskCreate(usbClientTask, "usb_client", 4096, nullptr, 3, nullptr);

  ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &xferOut));
  ESP_ERROR_CHECK(usb_host_transfer_alloc(DATA_BUF_SIZE, 0, &xferIn));
  ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &xferCtrl));

  Serial.println("En attente d'une clé USB sur le port OTG...");
}

void loop() {
  UsbEvent ev;
  if (xQueueReceive(eventQueue, &ev, portMAX_DELAY) != pdTRUE) return;

  if (ev.connected) {
    handleNewDevice(ev.address);
  } else {
    closeDevice();
    Serial.println("Clé retirée. En attente d'une clé USB...");
  }
}
