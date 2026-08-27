#include "room.hpp"
#include "save.hpp"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "ROOM";
static const char* K_TOY = "toy";     // item id of the toy that is out ("" = none)

void Room::init(SaveStore& save)
{
    save_ = &save;
    save_->loadStr(K_TOY, toy_, sizeof toy_, "");
    if (toy_[0]) ESP_LOGI(TAG, "toy out: '%s'", toy_);
}

void Room::setToy(const char* id)
{
    // Putting a toy out is a deliberate, rare act, so it persists immediately rather than
    // waiting for a flush -- there is no burst of these to batch.
    if (!id) id = "";
    if (strcmp(id, toy_) == 0) return;
    strncpy(toy_, id, sizeof toy_ - 1);
    toy_[sizeof toy_ - 1] = '\0';
    save_->storeStr(K_TOY, toy_);
    ESP_LOGI(TAG, "toy out: '%s'", toy_[0] ? toy_ : "(none)");
}
