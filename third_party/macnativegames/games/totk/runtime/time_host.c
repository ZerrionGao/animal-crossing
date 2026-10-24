/* nn::time for the functions main imports, backed by the host clock and time zone.
   User and network system clocks both follow the (NTP-synchronised) host wall clock; the steady clock
   counts seconds of host continuous time with one source id per process. ClockSnapshot uses the public
   0xD0-byte layout because game code may read its fields through inline accessors. */
#include "runtime.h"
#include <CoreFoundation/CFTimeZone.h>
#include <mach/mach_time.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { int64_t value; uint8_t source[16]; } SteadyClockTimePoint;
typedef struct { int64_t offset; SteadyClockTimePoint point; } SystemClockContext;
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { int32_t day_of_week, day_of_year; char name[8]; uint8_t dst, pad[3]; int32_t utc_offset; } CalendarAdditionalInfo;
#ifdef GAME_ACNH
typedef struct { char bytes[0x24]; } LocationName;
typedef struct {
    uint32_t magic, version;
    char name[0x24];
    uint32_t data_length;
    uint8_t data[0x4000 - 0x30];
} AcnhTimeZoneRule;
_Static_assert(sizeof(LocationName) == 0x24 && sizeof(AcnhTimeZoneRule) == 0x4000, "nn::time timezone types");
#endif
typedef struct {
    SystemClockContext user, network; int64_t user_time, network_time;
    CalendarTime user_calendar, network_calendar; CalendarAdditionalInfo user_info, network_info;
    SteadyClockTimePoint steady; char location[0x24]; uint8_t automatic_correction, type, pad[2];
} ClockSnapshot;
_Static_assert(sizeof(SteadyClockTimePoint) == 0x18 && sizeof(CalendarTime) == 8 && sizeof(CalendarAdditionalInfo) == 0x18, "nn::time types");
_Static_assert(sizeof(ClockSnapshot) == 0xd0, "nn::time::ClockSnapshot");

#define R_NOT_INITIALIZED 0x74u  /* SDK StandardUserSystemClockContext 的未初始化错误 */
#define R_INVALID R_NOT_INITIALIZED
#define R_SOURCE_MISMATCH 0x18074u
#define ACNH_TIME_ZONE_RULE_MAGIC 0x41434e48u
#define ACNH_TIME_ZONE_RULE_VERSION 1u
#define R_CLOCK_CONTEXT_INVALID 0xcc74u
static uint8_t source_id[16];
#ifdef GAME_ACNH
static pthread_mutex_t clock_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned clock_init_refs;
static int adjustable_clock_ready;
static int64_t adjustable_wall_ns;
static int64_t adjustable_continuous_ns;
static int64_t realtime_nanoseconds(void);
static int64_t continuous_nanoseconds(void);
#else
static int initialized;
#endif
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void make_source(void) { arc4random_buf(source_id, sizeof source_id); }
#ifdef GAME_ACNH
static pthread_once_t time_offset_once = PTHREAD_ONCE_INIT;
static long time_offset;
static void read_time_offset(void) {
    const char *e = getenv("TOTK_TIME_OFFSET");
    time_offset = e ? atol(e) : 0;
    if (time_offset < 0) time_offset = 0;
}
#endif

static int64_t steady_seconds(void) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    return (int64_t)((__uint128_t)mach_continuous_time() * tb.numer / tb.denom / 1000000000u);
}
static SteadyClockTimePoint steady_now(void) { pthread_once(&once, make_source); SteadyClockTimePoint p = {steady_seconds(), {0}}; memcpy(p.source, source_id, 16); return p; }
static int64_t posix_now(void) {
#ifdef GAME_ACNH
    pthread_once(&time_offset_once, read_time_offset); /* 诊断用途：按秒平移游戏读取的墙钟 */
    return (int64_t)time(NULL) + time_offset;
#else
    static long off = -1; if (off < 0) { const char *e = getenv("TOTK_TIME_OFFSET"); off = e ? atol(e) : 0; if (off < 0) off = 0; }
    return (int64_t)time(NULL) + off;
#endif
}

static unsigned initialize(void) {
    pthread_once(&once, make_source);
#ifdef GAME_ACNH
    pthread_mutex_lock(&clock_lock);
    if (clock_init_refs == UINT_MAX) {
        pthread_mutex_unlock(&clock_lock);
        runtime_fail("ACNH time Initialize reference count overflow");
    }
    if (!clock_init_refs) {
        adjustable_wall_ns = realtime_nanoseconds();
        adjustable_continuous_ns = continuous_nanoseconds();
        adjustable_clock_ready = 1;
    }
    clock_init_refs++;
    pthread_mutex_unlock(&clock_lock);
#else
    initialized = 1;
#endif
    return 0;
}
static unsigned finalize(void) {
#ifdef GAME_ACNH
    pthread_mutex_lock(&clock_lock);
    if (!clock_init_refs) { pthread_mutex_unlock(&clock_lock); return R_INVALID; }
    if (!--clock_init_refs) adjustable_clock_ready = 0;
    pthread_mutex_unlock(&clock_lock);
#else
    initialized = 0;
#endif
    return 0;
}
static unsigned char is_initialized(void) {
#ifdef GAME_ACNH
    pthread_mutex_lock(&clock_lock);
    unsigned char result = clock_init_refs != 0;
    pthread_mutex_unlock(&clock_lock);
    return result;
#else
    return (unsigned char)initialized;
#endif
}
static unsigned user_time(int64_t *out) { if (!out) return R_INVALID; *out = posix_now(); return 0; }

#ifdef GAME_ACNH
static int64_t continuous_nanoseconds(void) {
#ifdef ACNH_TIME_TEST_CLOCKS
    extern int64_t acnh_time_test_continuous_ns(void);
    return acnh_time_test_continuous_ns();
#else
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (int64_t)((__uint128_t)mach_continuous_time() * tb.numer / tb.denom);
#endif
}
static int64_t realtime_nanoseconds(void) {
#ifdef ACNH_TIME_TEST_CLOCKS
    extern int64_t acnh_time_test_realtime_ns(void);
    int64_t wall = acnh_time_test_realtime_ns();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) runtime_fail("adjustable clock realtime sample");
    int64_t wall = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
    pthread_once(&time_offset_once, read_time_offset);
    __int128 adjusted = (__int128)wall + (__int128)time_offset * 1000000000LL;
    if (adjusted > INT64_MAX || adjusted < INT64_MIN) runtime_fail("adjustable clock offset overflow");
    return (int64_t)adjusted;
}
static void adjustable_user_adjust(void) {
    pthread_mutex_lock(&clock_lock);
    if (!clock_init_refs) {
        pthread_mutex_unlock(&clock_lock);
        runtime_fail("adjustable clock Adjust before Initialize");
    }
    adjustable_wall_ns = realtime_nanoseconds();
    adjustable_continuous_ns = continuous_nanoseconds();
    adjustable_clock_ready = 1;
    pthread_mutex_unlock(&clock_lock);
}
static uint32_t adjustable_user_time(int64_t *out) {
    if (!out) runtime_fail("adjustable clock GetCurrentTime null output");
    pthread_mutex_lock(&clock_lock);
    if (!clock_init_refs) {
        pthread_mutex_unlock(&clock_lock);
        return R_NOT_INITIALIZED;
    }
    if (!adjustable_clock_ready) {
        pthread_mutex_unlock(&clock_lock);
        return R_CLOCK_CONTEXT_INVALID;
    }
    int64_t wall = adjustable_wall_ns;
    int64_t continuous = adjustable_continuous_ns;
    pthread_mutex_unlock(&clock_lock);
    int64_t now = continuous_nanoseconds();
    __int128 current = (__int128)wall + ((__int128)now - continuous);
    __int128 seconds = current / 1000000000LL;
    if (seconds > INT64_MAX || seconds < INT64_MIN) runtime_fail("adjustable clock result overflow");
    *out = (int64_t)seconds;
    return 0;
}
#endif
/* StandardSteadyClock::now() returns the chrono-style time_point: the int64 seconds value only, 0 without a source
   (sdk+0x3d1d88 returns it in x0; returning the 0x18-byte struct made the host write through the caller's stale x8). */
static int64_t steady_clock_now(void) { return steady_now().value; }
#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md): the steady clock is read into a time point, and a span is only defined between
   points of the same clock source. */
static uint32_t steady_time_point(SteadyClockTimePoint *out) { if (!out) runtime_fail("steady clock time point"); *out = steady_now(); return 0; }
static uint32_t span_between_points(int64_t *out, const SteadyClockTimePoint *a, const SteadyClockTimePoint *b) {
    if (!out || !a || !b) runtime_fail("time span between points");
    if (memcmp(a->source, b->source, 16)) return 0x2a6u;          /* time: not comparable (different clock sources) */
    *out = b->value - a->value; return 0;
}
#endif

static void fill_calendar(CalendarTime *c, CalendarAdditionalInfo *info, int64_t posix, int local) {
    time_t t = (time_t)posix; struct tm tm;
    if (!(local ? localtime_r(&t, &tm) : gmtime_r(&t, &tm))) runtime_fail("calendar conversion");
    *c = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (info) {
        memset(info, 0, sizeof *info);
        info->day_of_week = tm.tm_wday; info->day_of_year = tm.tm_yday;
        snprintf(info->name, sizeof info->name, "%s", local && tm.tm_zone ? tm.tm_zone : "UTC");
        info->dst = local && tm.tm_isdst > 0; info->utc_offset = local ? (int32_t)tm.tm_gmtoff : 0;
    }
}
static unsigned to_calendar(CalendarTime *c, CalendarAdditionalInfo *info, const int64_t *posix) {
    if (!c || !posix) return R_INVALID; fill_calendar(c, info, *posix, 1); return 0;
}
#ifdef GAME_ACNH
static CFTimeZoneRef timezone_from_location(const LocationName *location) {
    if (!location) runtime_fail("ACNH timezone location null");
    const char *end = memchr(location->bytes, 0, sizeof location->bytes);
    if (!end || end == location->bytes) runtime_fail("ACNH timezone location malformed");
    CFStringRef name = CFStringCreateWithBytes(kCFAllocatorDefault,
        (const UInt8 *)location->bytes, (CFIndex)(end - location->bytes), kCFStringEncodingUTF8, false);
    if (!name) runtime_fail("ACNH timezone location encoding");
    CFTimeZoneRef zone = CFTimeZoneCreateWithName(kCFAllocatorDefault, name, false);
    CFRelease(name);
    if (!zone) runtime_fail("ACNH unknown timezone location");
    return zone;
}

static void acnh_location_name(LocationName *out) {
    if (!out) runtime_fail("ACNH timezone name output null");
    CFTimeZoneRef zone = CFTimeZoneCopySystem();
    if (!zone) runtime_fail("ACNH system timezone unavailable");
    CFStringRef name = CFTimeZoneGetName(zone);
    memset(out, 0, sizeof *out);
    if (!name || !CFStringGetCString(name, out->bytes, sizeof out->bytes, kCFStringEncodingUTF8)) {
        CFRelease(zone);
        runtime_fail("ACNH system timezone name does not fit LocationName");
    }
    CFRelease(zone);
}

static uint32_t acnh_read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t acnh_tzif_block_end(const uint8_t *data, size_t length, size_t start, size_t time_width) {
    if (start > length || length - start < 44 || memcmp(data + start, "TZif", 4))
        runtime_fail("ACNH malformed TZif header");
    const uint8_t *counts = data + start + 20;
    uint32_t gmt_count = acnh_read_be32(counts);
    uint32_t std_count = acnh_read_be32(counts + 4);
    uint32_t leap_count = acnh_read_be32(counts + 8);
    uint32_t time_count = acnh_read_be32(counts + 12);
    uint32_t type_count = acnh_read_be32(counts + 16);
    uint32_t char_count = acnh_read_be32(counts + 20);
    if (!type_count || type_count > 256 || !char_count ||
        (gmt_count && gmt_count != type_count) || (std_count && std_count != type_count))
        runtime_fail("ACNH malformed TZif counts");
    uint64_t block_size = (uint64_t)time_count * time_width + time_count +
        (uint64_t)type_count * 6 + char_count + (uint64_t)leap_count * (time_width + 4) + std_count + gmt_count;
    uint64_t end = (uint64_t)start + 44 + block_size;
    if (end > length) runtime_fail("ACNH truncated TZif block");
    return end;
}

static void acnh_validate_tzif(const uint8_t *data, size_t length) {
    if (!data || length < 44 || memcmp(data, "TZif", 4)) runtime_fail("ACNH malformed timezone rule data");
    uint8_t version = data[4];
    if (version == 0) {
        (void)acnh_tzif_block_end(data, length, 0, 4);
        return;
    }
    if (version != '2' && version != '3' && version != '4') runtime_fail("ACNH unsupported TZif version");
    uint64_t first_end = acnh_tzif_block_end(data, length, 0, 4);
    if (first_end > SIZE_MAX || first_end > length || length - (size_t)first_end < 44 ||
        data[(size_t)first_end + 4] != version)
        runtime_fail("ACNH malformed TZif extended header");
    uint64_t second_end = acnh_tzif_block_end(data, length, (size_t)first_end, 8);
    if (second_end > length || length - (size_t)second_end < 2 || data[(size_t)second_end] != '\n' ||
        data[length - 1] != '\n')
        runtime_fail("ACNH malformed TZif footer");
}

static uint32_t acnh_load_timezone_rule(AcnhTimeZoneRule *out, const LocationName *location) {
    if (!out) runtime_fail("ACNH timezone rule output null");
    CFTimeZoneRef zone = timezone_from_location(location);
    CFDataRef data = CFTimeZoneGetData(zone);
    CFIndex length = data ? CFDataGetLength(data) : 0;
    if (!data || length <= 0 || (uint64_t)length > sizeof out->data) {
        CFRelease(zone);
        runtime_fail("ACNH timezone rule data missing or oversized");
    }
    acnh_validate_tzif(CFDataGetBytePtr(data), (size_t)length);
    memset(out, 0, sizeof *out);
    out->magic = ACNH_TIME_ZONE_RULE_MAGIC;
    out->version = ACNH_TIME_ZONE_RULE_VERSION;
    memcpy(out->name, location->bytes, sizeof out->name);
    out->data_length = (uint32_t)length;
    memcpy(out->data, CFDataGetBytePtr(data), (size_t)length);
    CFRelease(zone);
    return 0;
}

static CFTimeZoneRef acnh_timezone_from_rule(const AcnhTimeZoneRule *rule) {
    if (!rule || rule->magic != ACNH_TIME_ZONE_RULE_MAGIC || rule->version != ACNH_TIME_ZONE_RULE_VERSION ||
        !rule->data_length || rule->data_length > sizeof rule->data ||
        !memchr(rule->name, 0, sizeof rule->name) || !rule->name[0]) {
        runtime_fail("ACNH malformed timezone rule");
    }
    acnh_validate_tzif(rule->data, rule->data_length);
    const char *end = memchr(rule->name, 0, sizeof rule->name);
    CFStringRef name = CFStringCreateWithBytes(kCFAllocatorDefault,
        (const UInt8 *)rule->name, (CFIndex)(end - rule->name), kCFStringEncodingUTF8, false);
    if (!name) runtime_fail("ACNH timezone rule name encoding");
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, rule->data, (CFIndex)rule->data_length);
    if (!data) { CFRelease(name); runtime_fail("ACNH timezone rule data allocation"); }
    CFTimeZoneRef zone = CFTimeZoneCreate(kCFAllocatorDefault, name, data);
    CFRelease(data);
    CFRelease(name);
    if (!zone) runtime_fail("ACNH malformed timezone rule data");
    return zone;
}

static uint32_t acnh_to_calendar_with_rule(CalendarTime *calendar, CalendarAdditionalInfo *info,
                                             const int64_t *posix, const AcnhTimeZoneRule *rule) {
    if (!calendar || !posix) runtime_fail("ACNH timezone calendar output or PosixTime null");
    CFTimeZoneRef zone = acnh_timezone_from_rule(rule);
    CFAbsoluteTime absolute = (CFAbsoluteTime)*posix - kCFAbsoluteTimeIntervalSince1970;
    CFTimeInterval raw_offset = CFTimeZoneGetSecondsFromGMT(zone, absolute);
    if (raw_offset < INT32_MIN || raw_offset > INT32_MAX) {
        CFRelease(zone);
        runtime_fail("ACNH timezone offset out of range");
    }
    int32_t offset = (int32_t)raw_offset;
    __int128 local_seconds = (__int128)*posix + offset;
    if (local_seconds < INT64_MIN || local_seconds > INT64_MAX) {
        CFRelease(zone);
        runtime_fail("ACNH local calendar time overflow");
    }
    time_t local_time = (time_t)(int64_t)local_seconds;
    if ((__int128)local_time != local_seconds) {
        CFRelease(zone);
        runtime_fail("ACNH local calendar time_t out of range");
    }
    struct tm tm;
    if (!gmtime_r(&local_time, &tm) || tm.tm_year + 1900 < INT16_MIN || tm.tm_year + 1900 > INT16_MAX) {
        CFRelease(zone);
        runtime_fail("ACNH timezone calendar conversion");
    }
    *calendar = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday,
        (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (info) {
        CFStringRef abbreviation = CFTimeZoneCopyAbbreviation(zone, absolute);
        memset(info, 0, sizeof *info);
        info->day_of_week = tm.tm_wday;
        info->day_of_year = tm.tm_yday;
        if (!abbreviation || !CFStringGetCString(abbreviation, info->name, sizeof info->name, kCFStringEncodingUTF8)) {
            if (abbreviation) CFRelease(abbreviation);
            CFRelease(zone);
            runtime_fail("ACNH timezone abbreviation does not fit CalendarAdditionalInfo");
        }
        CFRelease(abbreviation);
        info->dst = CFTimeZoneIsDaylightSavingTime(zone, absolute) ? 1 : 0;
        info->utc_offset = offset;
    }
    CFRelease(zone);
    return 0;
}

/* 把日历字段当成 UTC 得到“本地钟面秒”。timegm 会把 2 月 31 日这类日期进位，进位后的字段对不上就不是合法本地时间。 */
static int naive_local_seconds(const CalendarTime *calendar, int64_t *out) {
    if (calendar->month < 1 || calendar->month > 12 || calendar->day < 1 || calendar->day > 31 ||
        calendar->hour < 0 || calendar->hour > 23 || calendar->minute < 0 || calendar->minute > 59 ||
        calendar->second < 0 || calendar->second > 59) return 0;
    struct tm tm = {0};
    tm.tm_year = calendar->year - 1900; tm.tm_mon = calendar->month - 1; tm.tm_mday = calendar->day;
    tm.tm_hour = calendar->hour; tm.tm_min = calendar->minute; tm.tm_sec = calendar->second;
    time_t naive = timegm(&tm);
    struct tm back;
    if (!gmtime_r(&naive, &back)) return 0;
    if (back.tm_year + 1900 != calendar->year || back.tm_mon + 1 != calendar->month || back.tm_mday != calendar->day ||
        back.tm_hour != calendar->hour || back.tm_min != calendar->minute || back.tm_sec != calendar->second) return 0;
    *out = (int64_t)naive;
    return 1;
}

static void insert_posix(int64_t *found, int *count, int64_t value) {
    for (int i = 0; i < *count; i++) if (found[i] == value) return;
    if (*count >= 4) return;
    int i = *count;
    while (i > 0 && found[i - 1] > value) { found[i] = found[i - 1]; i--; }
    found[i] = value;
    (*count)++;
}

/* 同一本地时间在夏令时回拨时对应两个时间戳，在拨快的空档里一个都没有。空档返回 0 个，不中止进程。 */
static uint32_t acnh_to_posix_with_rule(int *out_count, int64_t *out_array, int max_count,
                                         const CalendarTime *calendar, const AcnhTimeZoneRule *rule) {
    if (!out_count || !calendar || max_count < 0 || (max_count > 0 && !out_array))
        runtime_fail("ACNH ToPosixTime arguments");
    int64_t naive = 0;
    if (!naive_local_seconds(calendar, &naive)) { *out_count = 0; return 0; }
    CFTimeZoneRef zone = acnh_timezone_from_rule(rule);
    int64_t found[4]; int nfound = 0;
    for (int64_t offset = -14 * 3600; offset <= 14 * 3600; offset += 60) {
        int64_t posix = naive - offset;
        CFAbsoluteTime absolute = (CFAbsoluteTime)posix - kCFAbsoluteTimeIntervalSince1970;
        CFTimeInterval actual = CFTimeZoneGetSecondsFromGMT(zone, absolute);
        if (actual < (CFTimeInterval)INT32_MIN || actual > (CFTimeInterval)INT32_MAX) continue;
        if (posix + (int64_t)actual == naive) insert_posix(found, &nfound, posix);
    }
    CFRelease(zone);
    int write = nfound < max_count ? nfound : max_count;
    if (write > 0) memcpy(out_array, found, (size_t)write * sizeof *found);
    *out_count = write;
    return 0;
}
#endif
static CalendarTime to_calendar_utc(const int64_t *posix) { CalendarTime c; fill_calendar(&c, NULL, posix ? *posix : 0, 0); return c; }
static int64_t to_posix_utc(const CalendarTime *c) {
    struct tm tm = {0}; tm.tm_year = c->year - 1900; tm.tm_mon = c->month - 1; tm.tm_mday = c->day; tm.tm_hour = c->hour; tm.tm_min = c->minute; tm.tm_sec = c->second;
    return (int64_t)timegm(&tm);
}

static void location_name(char out[0x24]) {
    memset(out, 0, 0x24); char link[512]; ssize_t n = readlink("/etc/localtime", link, sizeof link - 1);
    if (n > 0) { link[n] = 0; const char *z = strstr(link, "zoneinfo/"); snprintf(out, 0x24, "%s", z ? z + 9 : "UTC"); }
    else snprintf(out, 0x24, "UTC");
}
static void snapshot_init(ClockSnapshot *s) { memset(s, 0, sizeof *s); }
static unsigned snapshot_create(ClockSnapshot *s) {
    if (!s) return R_INVALID;
    SteadyClockTimePoint steady = steady_now(); int64_t now = posix_now();
    memset(s, 0, sizeof *s);
    s->user = (SystemClockContext){now - steady.value, steady}; s->network = s->user;
    s->user_time = now; s->network_time = now;
    fill_calendar(&s->user_calendar, &s->user_info, now, 1); fill_calendar(&s->network_calendar, &s->network_info, now, 1);
    s->steady = steady; location_name(s->location); s->automatic_correction = 1; s->type = 0;
    return 0;
}
static unsigned span_between(int64_t *out_ns, const ClockSnapshot *a, const ClockSnapshot *b) {
    if (!out_ns || !a || !b) return R_INVALID;
    if (memcmp(a->steady.source, b->steady.source, 16)) return R_SOURCE_MISMATCH;
    *out_ns = (b->steady.value - a->steady.value) * 1000000000LL; return 0;
}
/* Change of the user clock offset between snapshots, i.e. manual adjustments, as a TimeSpan. */
static int64_t user_difference(const ClockSnapshot *a, const ClockSnapshot *b) {
    if (memcmp(a->user.point.source, b->user.point.source, 16)) return 0;
    return (b->user.offset - a->user.offset) * 1000000000LL;
}

void *runtime_time_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn4time10InitializeEv", initialize}, {"_ZN2nn4time8FinalizeEv", finalize}, {"_ZN2nn4time13IsInitializedEv", is_initialized},
#ifdef GAME_ACNH
        {"_ZN2nn4time25AdjustableUserSystemClock6AdjustEv", adjustable_user_adjust},
        {"_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", adjustable_user_time},
        {"_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", acnh_location_name},
        {"_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", acnh_load_timezone_rule},
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE", acnh_to_calendar_with_rule},
        {"_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", acnh_to_posix_with_rule},
#endif
        {"_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", user_time},
        {"_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", user_time},
        {"_ZN2nn4time19StandardSteadyClock3nowEv", steady_clock_now},
#ifdef GAME_BOTW
        {"_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE", steady_time_point},
        {"_ZN2nn4time14GetSpanBetweenEPlRKNS0_20SteadyClockTimePointES4_", span_between_points},
#endif
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", to_calendar},
        {"_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", to_calendar_utc},
        {"_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", to_posix_utc},
        {"_ZN2nn4time13ClockSnapshotC1Ev", snapshot_init},
        {"_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", snapshot_create},
        {"_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", span_between},
        {"_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", user_difference},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
