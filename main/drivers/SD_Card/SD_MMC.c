#include "SD_MMC.h"

#define EXAMPLE_MAX_CHAR_SIZE    64
#define MOUNT_POINT "/sdcard"

static const char *SD_TAG = "SD";

uint32_t Flash_Size = 0;
uint32_t SDCard_Size = 0;

// Handle of the card mounted at boot (NULL when no card was present). Exposed so the
// runtime presence watcher (engine/sdwatch) can health-check it.
static sdmmc_card_t *s_card = NULL;
sdmmc_card_t *SD_GetCard(void) { return s_card; }

// Set when the card ANSWERED on the bus but its filesystem would not mount -- the one
// failure a format would actually fix. Kept apart from "no card at all" so the SD Card
// screen can tell the player which of the two they are looking at, and so the format
// button is only offered where it can help.
static bool s_mount_failed = false;


esp_err_t s_example_write_file(const char *path, char *data)
{
    ESP_LOGI(SD_TAG, "Opening file %s", path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(SD_TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }
    fprintf(f, data);
    fclose(f);
    ESP_LOGI(SD_TAG, "File written");

    return ESP_OK;
}

esp_err_t s_example_read_file(const char *path)
{
    ESP_LOGI(SD_TAG, "Reading file %s", path);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(SD_TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }
    char line[EXAMPLE_MAX_CHAR_SIZE];
    fgets(line, sizeof(line), f);
    fclose(f);

    // strip newline
    char *pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(SD_TAG, "Read from file: '%s'", line);

    return ESP_OK;
}


// Bus + slot wiring, shared by the boot mount and the explicit format below so the two can
// never drift apart on a pin or a bus width.
static void sd_bus_config(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    sdmmc_host_t h = SDMMC_HOST_DEFAULT();
    *host = h;

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdmmc_slot_config_t s = SDMMC_SLOT_CONFIG_DEFAULT();
    *slot = s;
    slot->width = 1;          // 1-wire  / 4-wire   slot->width = 4;

    slot->clk = CONFIG_EXAMPLE_PIN_CLK;
    slot->cmd = CONFIG_EXAMPLE_PIN_CMD;
    slot->d0 = CONFIG_EXAMPLE_PIN_D0;
    slot->d1 = CONFIG_EXAMPLE_PIN_D1;
    slot->d2 = CONFIG_EXAMPLE_PIN_D2;
    slot->d3 = CONFIG_EXAMPLE_PIN_D3;

    // Enable internal pullups on enabled pins. The internal pullups are insufficient however, please make sure 10k external pullups are connected on the bus. This is for debug / example purpose only.
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

// max_files: each mounted mod pack (game/engine/pakfs) holds ONE of these slots open for the
// whole session (up to 4), on top of the conversation scan's cross-frame dir handle and
// transient sprite/config reads -- 5 was sized before packs existed.
#define SD_MAX_FILES  10
#define SD_ALLOC_UNIT (16 * 1024)

static void sd_note_mounted(sdmmc_card_t *card)
{
    s_card = card;
    s_mount_failed = false;
    SDCard_Size = ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024);
}

void SD_Init(void)
{
    esp_err_t ret;

    // Idempotent: the deep-sleep timer-wake path mounts the card headlessly (the creature
    // roster can live in a mod pack under /sdcard/mods), and if a wake trigger fires it then
    // falls through to app_main, which calls this again. A second esp_vfs_fat_sdmmc_mount()
    // on the same mount point fails, and the convenience wrapper tears the host down on its
    // way out -- taking the working card with it.
    if (s_card != NULL) return;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        // NEVER FORMAT ON A FAILED MOUNT. This used to be true, inherited from the vendor
        // example, and it made "the filesystem did not parse" mean "repartition the card".
        // The card is the player's: it carries their mod packs, and the update images the
        // System Update screen reads. A tired contact or a half-finished write on the PC is
        // enough to fail a mount, and the repair for that is a retry or a reseat -- not a
        // wipe nobody asked for and nobody can undo.
        //
        // It also became reachable unattended: the deep-sleep poll mounts the card every 15
        // minutes with nobody holding the device, so a card that started failing in a drawer
        // would have been erased with nobody watching. The data partition already mounts
        // with false (see gamedata.cpp); this brings the card in line.
        //
        // Formatting still exists, but only where a human asked for it by name:
        // Settings -> SYSTEM -> SD Card, which calls SD_Format() below.
        .format_if_mount_failed = false,
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = SD_ALLOC_UNIT
    };
    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(SD_TAG, "Initializing SD card");

    // Use settings defined above to initialize SD card and mount FAT filesystem.
    // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
    // Please check its source code and implement error recovery when developing production applications.
    ESP_LOGI(SD_TAG, "Using SPI peripheral");

    sdmmc_host_t host;
    sdmmc_slot_config_t slot_config;
    sd_bus_config(&host, &slot_config);

    ESP_LOGI(SD_TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            // The card answered on the bus; its filesystem did not parse. That is the one
            // failure a format would fix, so remember it -- the SD Card screen tells this
            // apart from "nothing inserted" and only offers the format for this case.
            s_mount_failed = true;
            ESP_LOGE(SD_TAG, "Failed to mount filesystem. The card is present but carries no "
                             "readable filesystem; format it from Settings -> SYSTEM -> SD Card "
                             "if you are sure it holds nothing you want.");
        } else {
            s_mount_failed = false;
            ESP_LOGE(SD_TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(SD_TAG, "Filesystem mounted");

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);
    sd_note_mounted(card);
}

bool SD_MountFailed(void) { return s_mount_failed; }

// Erase the card and put a fresh FAT on it. ONLY ever called from the player-facing SD Card
// screen -- nothing on a boot or wake path may call this.
//
// Two routes, because the interesting case is the one the convenience API cannot serve:
//
//   mounted card   -> esp_vfs_fat_sdcard_format(), which reformats in place.
//   unmounted card -> mount it WITH format_if_mount_failed, the only path in the SDK that
//                     writes a filesystem onto a volume that would not mount. This is the
//                     behaviour SD_Init() just gave up, kept alive here where a human asked
//                     for it by name; esp_vfs_fat_sdcard_format() is no use in this case,
//                     since it requires a mounted volume and the whole problem is that the
//                     volume will not mount.
//
// EITHER WAY THE CALLER MUST RESTART THE DEVICE. The in-place route force-unmounts the
// volume (f_mount(0, ...)), which invalidates every open handle on it without asking -- and
// each mounted mod pack holds one. Even on success, everything the card contributes
// (creatures, foods, mods, update images) is boot-time state that is now wrong.
esp_err_t SD_Format(void)
{
    if (s_card != NULL) {
        ESP_LOGW(SD_TAG, "formatting the mounted card at the player's request");
        esp_err_t e = esp_vfs_fat_sdcard_format(MOUNT_POINT, s_card);
        if (e != ESP_OK) ESP_LOGE(SD_TAG, "format failed: %s", esp_err_to_name(e));
        return e;
    }

    ESP_LOGW(SD_TAG, "formatting an unmounted card at the player's request");
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,     // the point of this branch
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = SD_ALLOC_UNIT
    };
    sdmmc_host_t host;
    sdmmc_slot_config_t slot_config;
    sd_bus_config(&host, &slot_config);

    sdmmc_card_t *card = NULL;
    esp_err_t e = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (e != ESP_OK) {
        // No card at all lands here too, which is why the screen greys the button out unless
        // the card is either mounted or known-unreadable.
        ESP_LOGE(SD_TAG, "format failed: %s", esp_err_to_name(e));
        return e;
    }
    sd_note_mounted(card);
    return ESP_OK;
}

// Probe-only "is a card answering on the bus?" for mid-session INSERTION detection.
// No mount and no format (unlike SD_Init), so it is safe to call repeatedly. Only for
// use while nothing is mounted: after a failed SD_Init the convenience mount has cleaned
// the host up again, so the first probe brings host+slot up once and keeps them.
bool SD_Probe_Insertion(void)
{
    static bool hostReady = false;
    static sdmmc_card_t probe;

    if (!hostReady) {
        esp_err_t err = sdmmc_host_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.width = 1;
        slot_config.clk = CONFIG_EXAMPLE_PIN_CLK;
        slot_config.cmd = CONFIG_EXAMPLE_PIN_CMD;
        slot_config.d0  = CONFIG_EXAMPLE_PIN_D0;
        slot_config.d1  = CONFIG_EXAMPLE_PIN_D1;
        slot_config.d2  = CONFIG_EXAMPLE_PIN_D2;
        slot_config.d3  = CONFIG_EXAMPLE_PIN_D3;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot_config) != ESP_OK) return false;
        hostReady = true;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    return sdmmc_card_init(&host, &probe) == ESP_OK;
}
void Flash_Searching(void)
{
    if(esp_flash_get_physical_size(NULL, &Flash_Size) == ESP_OK)
    {
        Flash_Size = Flash_Size / (uint32_t)(1024 * 1024);
        printf("Flash size: %ld MB\n", Flash_Size);
    }
    else{
        printf("Get flash size failed\n");
    }
}


FILE* Open_File(const char *file_path) {
    ESP_LOGI(SD_TAG, "Attempting to open file: %s", file_path);
    FILE *fp = fopen(file_path, "rb"); // Open the MP3 file in binary mode
    if (fp == NULL) {
        ESP_LOGE(SD_TAG, "Failed to open file %s. Error: %s", file_path, strerror(errno));
    }
    else
        printf("File %s was successfully opened. \r\n", file_path);
    return fp; 
}

#define MAX_FILE_NAME_SIZE 100  // Define maximum file name size
#define MAX_PATH_SIZE 512      // Define a larger size for the full path
uint16_t Folder_retrieval(const char* directory, const char* fileExtension, char File_Name[][MAX_FILE_NAME_SIZE], uint16_t maxFiles)    
{
    DIR *dir = opendir(directory);  // Opens the specified directory
    if (dir == NULL) {
        ESP_LOGE(SD_TAG, "Path: <%s> does not exist", directory);  
        return 0; 
    }

    uint16_t fileCount = 0;  // File counter
    struct dirent *entry;    // Directory entry pointer

    // 遍历目录中的所有条目
    while ((entry = readdir(dir)) != NULL && fileCount < maxFiles) {
        // Skip "." and ".." Special directory
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        const char *dot = strrchr(entry->d_name, '.');  
        if (dot != NULL && dot != entry->d_name) {  


            if (strcasecmp(dot, fileExtension) == 0) { 

                strncpy(File_Name[fileCount], entry->d_name, MAX_FILE_NAME_SIZE - 1);
                File_Name[fileCount][MAX_FILE_NAME_SIZE - 1] = '\0'; 

 
                char filePath[MAX_PATH_SIZE];
                snprintf(filePath, MAX_PATH_SIZE, "%s/%s", directory, entry->d_name);

                printf("File found: %s\r\n", filePath); 
                fileCount++;  
            }
        }
        else{
            // If the extension name is not found, you can output debugging information
            // printf("No extension found for file: %s\r\n", entry->d_name);
        }
    }

    closedir(dir);  // 

    if (fileCount > 0) {
        ESP_LOGI(SD_TAG, "Retrieved %d files with extension '%s'", fileCount, fileExtension);  // 
    } else {
        ESP_LOGW(SD_TAG, "No files with extension '%s' found in directory: %s", fileExtension, directory);  // 
    }

    return fileCount;  
}
