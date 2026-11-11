// Liste dans le moniteur série tous les fichiers d'une clé USB branchée
// sur le port USB OTG de l'ESP32-S3-DevKitC-1 v1.1 (ESP-IDF + usb_host_msc).

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#define MOUNT_POINT "/usb"

static const char *TAG = "usb_list";
static QueueHandle_t app_queue;

typedef struct {
    bool connected;
    uint8_t address;                  // valide si connected
    msc_host_device_handle_t handle;  // valide si !connected
} app_msg_t;

// Appelé par le pilote MSC : on transmet l'événement à la tâche principale.
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    app_msg_t msg = {0};
    if (event->event == MSC_DEVICE_CONNECTED) {
        msg.connected = true;
        msg.address = event->device.address;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        msg.connected = false;
        msg.handle = event->device.handle;
    } else {
        return;
    }
    xQueueSend(app_queue, &msg, portMAX_DELAY);
}

// Tâche de la pile hôte USB : doit tourner en permanence.
static void usb_host_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);  // signale que l'hôte USB est prêt

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static char path[512];
static unsigned file_count, dir_count;
static unsigned long long total_bytes;

// Parcours récursif : `path` contient le dossier courant, sa longueur est `len`.
static void list_dir(size_t len, int depth)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        printf("%*s[impossible d'ouvrir %s]\n", depth * 2, "", path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        int n = snprintf(path + len, sizeof(path) - len, "/%s", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path) - len) {
            printf("%*s[chemin trop long : %s]\n", depth * 2, "", entry->d_name);
            path[len] = '\0';
            continue;
        }

        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("%*s%s/\n", depth * 2, "", entry->d_name);
            dir_count++;
            list_dir(len + n, depth + 1);
        } else {
            long size = (stat(path, &st) == 0) ? (long)st.st_size : -1;
            printf("%*s%s  (%ld octets)\n", depth * 2, "", entry->d_name, size);
            file_count++;
            if (size > 0) total_bytes += size;
        }
        path[len] = '\0';
    }
    closedir(dir);
}

static void list_usb_key(void)
{
    file_count = dir_count = 0;
    total_bytes = 0;
    strcpy(path, MOUNT_POINT);

    printf("\n===== Contenu de la clé USB =====\n");
    list_dir(strlen(path), 0);
    printf("===== %u fichier(s), %u dossier(s), %llu octets =====\n\n",
           file_count, dir_count, total_bytes);
}

void app_main(void)
{
    app_queue = xQueueCreate(5, sizeof(app_msg_t));

    xTaskCreate(usb_host_task, "usb_host", 4096, xTaskGetCurrentTaskHandle(), 2, NULL);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,  // (sic) nom du champ dans la bibliothèque
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_config));

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,  // ne jamais effacer la clé
        .max_files = 3,
        .allocation_unit_size = 8192,
    };

    msc_host_device_handle_t device = NULL;
    msc_host_vfs_handle_t vfs = NULL;

    printf("En attente d'une clé USB sur le port OTG...\n");

    while (true) {
        app_msg_t msg;
        xQueueReceive(app_queue, &msg, portMAX_DELAY);

        if (msg.connected) {
            ESP_LOGI(TAG, "Clé détectée (adresse %d)", msg.address);
            if (msc_host_install_device(msg.address, &device) != ESP_OK) {
                ESP_LOGE(TAG, "Impossible d'initialiser la clé");
                device = NULL;
                continue;
            }
            esp_err_t err = msc_host_vfs_register(device, MOUNT_POINT, &mount_config, &vfs);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Montage impossible (%s). La clé est-elle en FAT32 ?",
                         esp_err_to_name(err));
                vfs = NULL;
                continue;
            }
            list_usb_key();
        } else {
            if (vfs) {
                msc_host_vfs_unregister(vfs);
                vfs = NULL;
            }
            if (device) {
                msc_host_uninstall_device(device);
                device = NULL;
            }
            printf("Clé retirée. En attente d'une clé USB...\n");
        }
    }
}
