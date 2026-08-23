#include "scene_about.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/clock.hpp"
#include "engine/battery.hpp"
#include "engine/fw_update.hpp"    // fw_current_version
#include "engine/pakfs.hpp"        // mounted mod packs
#include "sim/gamedata.hpp"       // gd_sd_count (what the loose-SD layer contributed)
#include <dirent.h>
#include "ui/widgets.hpp"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include "ST7789.h"      // Touch_Model (which controller answered at boot)
#include "PCF85063.h"    // PCF85063_Lost_Clock
#include "SD_MMC.h"      // SDCard_Size, Flash_Size (MB)
}

// Layout: a scrolling list of label/value rows. Values are right-aligned to VAL_R so the
// numbers form a column no matter how long the labels get.
static const int LIST_Y = 56, ROW_H = 22;
static const int LBL_X = 14, VAL_R = GAME_W - 14;
static const float REFRESH_S = 1.0f;   // how often the live rows (power, memory, clock) update

void SceneAbout::add(Kind k, const char* label, const char* fmt, ...)
{
    if (n_ >= MAX_ROWS) return;
    Row& r = rows_[n_++];
    r.kind = k;
    snprintf(r.label, sizeof r.label, "%s", label ? label : "");
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(r.value, sizeof r.value, fmt, ap);
        va_end(ap);
    } else {
        r.value[0] = '\0';
    }
}

// Human sizes. Everything here is either MB from a driver global or bytes from the heap, so
// two helpers rather than one generic formatter.
static void fmt_mb(char* out, size_t n, uint32_t mb)
{
    if (mb == 0)          snprintf(out, n, "none");
    else if (mb >= 1024)  snprintf(out, n, "%.1f GB", mb / 1024.0f);
    else                  snprintf(out, n, "%u MB", (unsigned)mb);
}

void SceneAbout::rebuild()
{
    n_ = 0;

    // --- firmware -----------------------------------------------------------------------
    const esp_app_desc_t* d = esp_app_get_description();
    add(Header, "FIRMWARE", nullptr);
    add(Field, "Version", "%s", fw_current_version());
    add(Field, "Built",   "%s %s", d->date, d->time);
    add(Field, "ESP-IDF", "%s", d->idf_ver);
    // Which A/B slot is running, and whether the bootloader is still holding a rollback over
    // it (only true on the first boot after an SD update, before fw_confirm_running_image).
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_VALID;
    if (run) esp_ota_get_state_partition(run, &st);
    add(Field, "Slot", "%s%s", run ? run->label : "?",
        st == ESP_OTA_IMG_PENDING_VERIFY ? " (verifying)" : "");

    // --- hardware -----------------------------------------------------------------------
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    add(Header, "HARDWARE", nullptr);
    add(Field, "Chip",  "ESP32-S3 rev v%d.%d", chip.revision / 100, chip.revision % 100);
    add(Field, "Cores", "%d", chip.cores);
    char buf[24];
    fmt_mb(buf, sizeof buf, Flash_Size);
    add(Field, "Flash", "%s", buf);
    size_t psram = esp_psram_get_size();
    add(Field, "PSRAM", "%.0f MB", psram / (1024.0f * 1024.0f));
    // The two board revisions are identical except for this chip, so it doubles as the
    // board's identity (see docs: V1 = CST328, V2 = CST3530).
    add(Field, "Touch", "%s", Touch_Model);
    fmt_mb(buf, sizeof buf, SDCard_Size);
    add(Field, "SD card", "%s", buf);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    add(Field, "MAC", "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // --- power --------------------------------------------------------------------------
    const BatteryState b = battery_state();
    add(Header, "POWER", nullptr);
    add(Field, "Battery", "%.2f V", b.volts);
    if (app().debugOverlay) add(Field, "Level", "%d %% (curve %d)", b.pct, b.pctRaw);
    else                    add(Field, "Level", "%d %%", b.pct);
    add(Field, "Charger", "%s", b.full     ? "charged"
                              : b.charging ? "charging"
                                           : "not plugged in");
    // What a full pack reads on this board, charging / on battery. The gauge measures both
    // (see engine/battery.cpp) and slides the discharge curve onto them, so these two numbers
    // explain any percentage it reports -- worth seeing when one looks wrong.
    add(Field, "Full reads", "%.2f / %.2f V%s", b.vFull, b.vLoadFull,
        b.calibrated ? "" : " est");
    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    add(Field, "Awake", "%uh %02um", (unsigned)(up / 3600), (unsigned)(up / 60 % 60));

    // --- clock --------------------------------------------------------------------------
    const datetime_t t = clock_datetime();
    add(Header, "CLOCK", nullptr);
    add(Field, "RTC time", "%04d-%02d-%02d %02d:%02d",
        t.year, t.month, t.day, t.hour, t.minute);
    // The backup cell has no voltage sense anywhere on this board -- the only thing the RTC
    // can tell us is whether its oscillator ran continuously, which is exactly what the cell
    // is there to do. So that is what gets reported, and the note says so.
    const bool lost = PCF85063_Lost_Clock();
    add(Field, "Backup cell", "%s", lost ? "clock was lost" : "kept the clock");
    add(Note, nullptr, "The cell reports no level, only");
    add(Note, nullptr, "whether it kept time while off.");

    // --- memory -------------------------------------------------------------------------
    add(Header, "MEMORY", nullptr);
    add(Field, "Free RAM", "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    add(Field, "RAM low",  "%u KB",
        (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    if (psram)
        add(Field, "Free PSRAM", "%.1f MB",
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024.0f * 1024.0f));

    // --- mods ---------------------------------------------------------------------------
    // Two layers, reported separately because they fail differently. A .pak either mounts or
    // it doesn't; loose files fail per-entry and silently, so what matters there is the count
    // each system actually accepted -- a folder full of creatures showing 0 is the whole
    // reason this screen exists.
    add(Header, "MOD PACKS", nullptr);
    const int packs = pakfs_count();
    if (packs == 0) add(Field, "Mounted", "none");
    for (int i = 0; i < packs; i++) {
        const char* src = pakfs_source(i);
        if (!src) continue;
        const char* base = strrchr(src, '/');
        add(Field, pakfs_root(i), "%s", base ? base + 1 : src);
    }

    add(Header, "LOOSE SD FILES", nullptr);
    if (SDCard_Size == 0) {
        add(Field, "Card", "not mounted");
    } else {
        for (int i = 0; i < GD_SYS_COUNT; i++) {
            const GdSystem g = (GdSystem)i;
            add(Field, gd_sd_name(g), "%d", gd_sd_count(g));
        }
        // Conversations are the one system with nothing to report at boot: it scans the card
        // lazily, per question asked, so there is no load-time tally to keep. Counting the
        // files instead answers the same question -- did the card's conversations get seen --
        // and is measured once on entry rather than on the refresh timer.
        add(Field, "Conversations", "%d file%s", convFiles_, convFiles_ == 1 ? "" : "s");
        add(Field, "Total", "%d", gd_sd_total() + convFiles_);
    }
}

// Files under /sdcard/conversations/<pool>/ plus the current creature's own folder -- the
// same places ConversationSystem looks. One pass over a handful of directories; FAT opens are
// slow enough (~10 ms each) that this belongs on scene entry, not in rebuild().
static int count_dir_files(const char* path)
{
    DIR* d = opendir(path);
    if (!d) return 0;
    int n = 0;
    while (struct dirent* e = readdir(d))
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

int SceneAbout::countLooseConversations()
{
    int n = 0;
    // Pool folders are whatever the player created; walking the parent avoids duplicating
    // ConversationSystem's list of pool names here, where it would quietly go stale.
    DIR* d = opendir("/sdcard/conversations");
    if (d) {
        while (struct dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char sub[128];
            snprintf(sub, sizeof sub, "/sdcard/conversations/%s", e->d_name);
            n += count_dir_files(sub);
        }
        closedir(d);
    }
    const char* species = app().pet.state().creatureId;
    if (species && species[0]) {
        char sp[160];
        snprintf(sp, sizeof sp, "/sdcard/creatures/%s/conversations", species);
        n += count_dir_files(sp);
    }
    return n;
}

void SceneAbout::onEnter()
{
    list_.geom(0, LIST_Y, GAME_W, GAME_H - LIST_Y, ROW_H);   // before the first onInput
    list_.reset();
    refresh_ = 0.0f;
    convFiles_ = countLooseConversations();   // once: it walks the card
    rebuild();
}

void SceneAbout::update(float dt)
{
    // Half of this screen is live state (voltage, uptime, heap, the clock), so it is rebuilt
    // on a timer. A full rebuild is a couple of dozen snprintf calls -- cheaper than tracking
    // which rows are static.
    refresh_ -= dt;
    if (refresh_ <= 0.0f) {
        refresh_ = REFRESH_S;
        rebuild();
    }
}

void SceneAbout::render()
{
    fb.fillScreen(col::panel);
    gfx_text(16, 16, 2, col::accent, "About");
    draw_back();

    list_.geom(0, LIST_Y, GAME_W, GAME_H - LIST_Y, ROW_H);
    list_.beginClip();
    for (int i = list_.first(); i <= list_.last(n_); i++) {
        const Row& r = rows_[i];
        const Rect rr = list_.rowRect(i);
        if (r.kind == Header) {
            gfx_text(LBL_X, rr.y + 8, 1, col::accent, "%s", r.label);
            fb.drawFastHLine(LBL_X, rr.y + 19, GAME_W - 2 * LBL_X, rgb565(70, 76, 100));
        } else if (r.kind == Note) {
            gfx_text(LBL_X, rr.y + 4, 1, col::dim, "%s", r.value);
        } else {
            gfx_text(LBL_X, rr.y + 6, 1, col::dim, "%s", r.label);
            const int w = (int)strlen(r.value) * 6;      // size-1 cell is 6 px wide
            gfx_text(VAL_R - w, rr.y + 6, 1, col::white, "%s", r.value);
        }
    }
    list_.endClip();
    list_.drawScrollbar(n_);
}

void SceneAbout::onInput(const Input& in)
{
    list_.update(in, n_);       // drag/flick scrolling (no row is tappable here)

    if (in.pressed && kBack.contains(in)) {
        app().setScene(SceneId::Settings, Slide::Back);
        return;
    }
}
