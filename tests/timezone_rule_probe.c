#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../third_party/macnativegames/games/totk/runtime/runtime.h"

typedef struct { char bytes[0x24]; } LocationName;
typedef struct {
    uint32_t magic, version;
    char name[0x24];
    uint32_t data_length;
    uint8_t data[0x4000 - 0x30];
} TimeZoneRule;
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { int32_t day_of_week, day_of_year; char name[8]; uint32_t dst; int32_t utc_offset; } CalendarAdditionalInfo;
_Static_assert(sizeof(TimeZoneRule) == 0x4000, "TimeZoneRule ABI");
_Static_assert(sizeof(CalendarTime) == 8 && sizeof(CalendarAdditionalInfo) == 0x18, "calendar ABI");

typedef void (*GetDeviceNameFn)(LocationName *);
typedef uint32_t (*LoadRuleFn)(TimeZoneRule *, const LocationName *);
typedef uint32_t (*ToCalendarFn)(CalendarTime *, CalendarAdditionalInfo *, const int64_t *, const TimeZoneRule *);
typedef uint32_t (*ToPosixFn)(int *, int64_t *, int, const CalendarTime *, const TimeZoneRule *);

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[probe-fail] %s\n", message);
    exit(42);
}

static GetDeviceNameFn get_device_name;
static LoadRuleFn load_rule;
static ToCalendarFn to_calendar;
static ToPosixFn to_posix;

#ifdef GAME_ACNH
static void load_named_rule(TimeZoneRule *rule, const char *name) {
    LocationName location = {{0}};
    assert(strlen(name) < sizeof location.bytes);
    strcpy(location.bytes, name);
    assert(load_rule(rule, &location) == 0);
    assert(rule->magic == 0x41434e48u && rule->version == 1);
    assert(rule->data_length > 0 && rule->data_length <= sizeof rule->data);
    assert(strcmp(rule->name, name) == 0);
}

static void assert_calendar(const TimeZoneRule *rule, int64_t posix, int year, int month, int day,
                           int hour, int minute, int second, int offset, int dst, const char *abbreviation) {
    CalendarTime calendar = {0};
    CalendarAdditionalInfo info = {0};
    assert(to_calendar(&calendar, &info, &posix, rule) == 0);
    assert(calendar.year == year && calendar.month == month && calendar.day == day);
    assert(calendar.hour == hour && calendar.minute == minute && calendar.second == second);
    assert(info.utc_offset == offset && info.dst == (uint32_t)dst);
    assert(strcmp(info.name, abbreviation) == 0);
}
#endif

int main(int argc, char **argv) {
    get_device_name = (GetDeviceNameFn)runtime_time_override("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE");
    load_rule = (LoadRuleFn)runtime_time_override("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE");
    to_calendar = (ToCalendarFn)runtime_time_override("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE");
    to_posix = (ToPosixFn)runtime_time_override("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE");
#ifdef GAME_ACNH
    assert(get_device_name && load_rule && to_calendar && to_posix);
    if (argc > 1 && strcmp(argv[1], "to-posix-rule") == 0) {
        int count = -1;
        int64_t values[2] = {0x123456789LL, 0x23456789aLL};
        CalendarTime calendar = {2024, 3, 10, 7, 0, 0, 0};
        TimeZoneRule rule;
        load_named_rule(&rule, "UTC");
        assert(to_posix(&count, values, 2, &calendar, &rule) == 0);
        assert(count == 1 && values[0] == 1710054000LL && values[1] == 0x23456789aLL);
        load_named_rule(&rule, "Asia/Hong_Kong");
        calendar = (CalendarTime){2024, 3, 10, 15, 0, 0, 0};
        count = -1; values[0] = 0;
        assert(to_posix(&count, values, 2, &calendar, &rule) == 0);
        assert(count == 1 && values[0] == 1710054000LL);
        load_named_rule(&rule, "America/New_York");
        calendar = (CalendarTime){2024, 3, 10, 2, 30, 0, 0};
        count = -1; values[0] = 1; values[1] = 2;
        assert(to_posix(&count, values, 2, &calendar, &rule) == 0);
        assert(count == 0 && values[0] == 1 && values[1] == 2);
        calendar = (CalendarTime){2024, 11, 3, 1, 30, 0, 0};
        count = -1;
        assert(to_posix(&count, values, 2, &calendar, &rule) == 0);
        assert(count == 2 && values[0] == 1730611800LL && values[1] == 1730615400LL);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "invalid-name") == 0) {
        TimeZoneRule rule;
        LocationName location;
        memset(&location, 'X', sizeof location);
        (void)load_rule(&rule, &location);
        assert(!"malformed location must fail");
    }
    if (argc > 1 && strcmp(argv[1], "malformed-rule") == 0) {
        TimeZoneRule rule;
        load_named_rule(&rule, "UTC");
        rule.data[0] = 0;
        CalendarTime calendar;
        (void)to_calendar(&calendar, NULL, &(int64_t){0}, &rule);
        assert(!"malformed timezone data must fail");
    }
    if (argc > 1 && strcmp(argv[1], "oversized-rule") == 0) {
        TimeZoneRule rule;
        load_named_rule(&rule, "UTC");
        rule.data_length = sizeof rule.data + 1;
        CalendarTime calendar;
        (void)to_calendar(&calendar, NULL, &(int64_t){0}, &rule);
        assert(!"oversized timezone data must fail");
    }
    if (argc > 1 && strcmp(argv[1], "malformed-counts") == 0) {
        TimeZoneRule rule;
        load_named_rule(&rule, "UTC");
        memset(rule.data + 32, 0xff, 4);
        CalendarTime calendar;
        (void)to_calendar(&calendar, NULL, &(int64_t){0}, &rule);
        assert(!"out-of-range TZif counts must fail");
    }

    struct { LocationName name; uint8_t guard[16]; } name_buffer;
    memset(&name_buffer, 0xa5, sizeof name_buffer);
    get_device_name(&name_buffer.name);
    assert(name_buffer.name.bytes[0] && memchr(name_buffer.name.bytes, 0, sizeof name_buffer.name.bytes));
    for (size_t i = 0; i < sizeof name_buffer.guard; i++) assert(name_buffer.guard[i] == 0xa5);

    struct { TimeZoneRule rule; uint8_t guard[16]; } system_rule;
    memset(&system_rule, 0xa5, sizeof system_rule);
    assert(load_rule(&system_rule.rule, &name_buffer.name) == 0);
    for (size_t i = 0; i < sizeof system_rule.guard; i++) assert(system_rule.guard[i] == 0xa5);
    CalendarTime system_calendar = {0};
    CalendarAdditionalInfo system_info = {0};
    int64_t sample = 1710054000LL;
    assert(to_calendar(&system_calendar, &system_info, &sample, &system_rule.rule) == 0);
    setenv("TZ", "Pacific/Honolulu", 1);
    tzset();
    CalendarTime stable_calendar = {0};
    CalendarAdditionalInfo stable_info = {0};
    assert(to_calendar(&stable_calendar, &stable_info, &sample, &system_rule.rule) == 0);
    assert(!memcmp(&system_calendar, &stable_calendar, sizeof system_calendar));
    assert(!memcmp(&system_info, &stable_info, sizeof system_info));

    TimeZoneRule hong_kong;
    load_named_rule(&hong_kong, "Asia/Hong_Kong");
    TimeZoneRule copied_hong_kong;
    memcpy(&copied_hong_kong, &hong_kong, sizeof copied_hong_kong);
    CalendarTime hk_calendar = {0};
    CalendarAdditionalInfo hk_info = {0};
    assert(to_calendar(&hk_calendar, &hk_info, &sample, &copied_hong_kong) == 0);
    assert(hk_calendar.year == 2024 && hk_calendar.month == 3 && hk_calendar.day == 10);
    assert(hk_calendar.hour == 15 && hk_calendar.minute == 0 && hk_calendar.second == 0);
    assert(hk_info.utc_offset == 28800 && hk_info.dst == 0);

    TimeZoneRule utc;
    load_named_rule(&utc, "UTC");
    CalendarTime utc_without_info = {0};
    int64_t utc_sample = 1710054000LL;
    assert(to_calendar(&utc_without_info, NULL, &utc_sample, &utc) == 0);
    assert(utc_without_info.year == 2024 && utc_without_info.month == 3 && utc_without_info.day == 10);
    assert_calendar(&utc, 1710054000LL, 2024, 3, 10, 7, 0, 0, 0, 0, "GMT");
    TimeZoneRule new_york;
    load_named_rule(&new_york, "America/New_York");
    assert_calendar(&new_york, 1710053999LL, 2024, 3, 10, 1, 59, 59, -18000, 0, "EST");
    assert_calendar(&new_york, 1710054000LL, 2024, 3, 10, 3, 0, 0, -14400, 1, "EDT");
    assert_calendar(&new_york, 1730613599LL, 2024, 11, 3, 1, 59, 59, -14400, 1, "EDT");
    assert_calendar(&new_york, 1730613600LL, 2024, 11, 3, 1, 0, 0, -18000, 0, "EST");
#else
    (void)argc;
    (void)argv;
    assert(!get_device_name && !load_rule && !to_calendar && !to_posix);
#endif
    return 0;
}
