#include "Stats.h"

StatsManager& StatsManager::instance() {
    static StatsManager inst;
    return inst;
}

StatsManager::StatsManager() {
    _mutex = xSemaphoreCreateMutex();
}

void StatsManager::lock() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
}

void StatsManager::unlock() {
    xSemaphoreGive(_mutex);
}

Stats& StatsManager::get() {
    return _stats;
}
