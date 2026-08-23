#include "clock.hpp"
#include "drivers.hpp"   // datetime_t datetime (RTC global, refreshed by driver task)
#include <cstring>

// The RTC `datetime` global is written field-by-field by the core-0 driver task
// while we read it on core 1, so a plain copy can tear (e.g. new day, old month).
// Read it twice and retry until two consecutive snapshots agree.
static datetime_t read_stable(void)
{
    datetime_t a = datetime, b = datetime;
    for (int i = 0; i < 3 && memcmp(&a, &b, sizeof a) != 0; ++i) {
        a = datetime;
        b = datetime;
    }
    return a;
}

// days since 1970-01-01 (Howard Hinnant's civil-from-days, inverse)
static uint32_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + (int)doe - 719468);
}

uint32_t clock_epoch(const datetime_t& t)
{
    if ((int)t.year < CLOCK_YEAR_MIN || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31)
        return 0;
    uint32_t days = days_from_civil((int)t.year, t.month, t.day);
    return days * 86400u + (uint32_t)t.hour * 3600u + (uint32_t)t.minute * 60u + t.second;
}

uint32_t clock_now(void)
{
    datetime_t t = read_stable();   // torn-read-safe snapshot of the RTC global
    return clock_epoch(t);
}

uint32_t clock_elapsed(uint32_t since)
{
    uint32_t now = clock_now();
    return (now >= since) ? (now - since) : 0;
}

datetime_t clock_datetime(void) { return read_stable(); }
