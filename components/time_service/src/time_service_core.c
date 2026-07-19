#include "time_service_core.h"

#include <stddef.h>
#include <string.h>

static bool _is_leap_year(int year)
{
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

static int _days_in_month(int year, int month)
{
    static const uint8_t days[] = { 31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31
                                  };
    int result = days[month - 1];
    if (month == 2 && _is_leap_year(year))
    {
        result = 29;
    }
    return result;
}

static int64_t _days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned shifted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned day_of_year =
        (153U * shifted_month + 2U) / 5U + day - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
                                year_of_era / 100U + day_of_year;
    return era * 146097LL + (int64_t)day_of_era - 719468LL;
}

static void _civil_from_days(int64_t days, int *year, unsigned *month,
                             unsigned *day)
{
    days += 719468LL;
    const int64_t era = (days >= 0 ? days : days - 146096LL) / 146097LL;
    const unsigned day_of_era = (unsigned)(days - era * 146097LL);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
         day_of_era / 146096U) / 365U;
    int result_year = (int)year_of_era + (int)(era * 400LL);
    const unsigned day_of_year = day_of_era -
                                 (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    const unsigned shifted_month = (5U * day_of_year + 2U) / 153U;

    *day = day_of_year - (153U * shifted_month + 2U) / 5U + 1U;
    *month = shifted_month < 10U ? shifted_month + 3U : shifted_month - 9U;
    result_year += *month <= 2U;
    *year = result_year;
}

bool time_service_core_tm_valid(const struct tm *value)
{
    bool valid = value != NULL;
    int year = 0;
    int month = 0;
    if (valid)
    {
        const int64_t wide_year = (int64_t)value->tm_year + 1900LL;
        valid = wide_year >= 1 && wide_year <= 9999;
        if (valid)
        {
            year = (int)wide_year;
            month = value->tm_mon + 1;
        }
    }
    if (valid)
    {
        valid = month >= 1 && month <= 12;
    }
    if (valid)
    {
        valid = value->tm_mday >= 1 &&
                value->tm_mday <= _days_in_month(year, month) &&
                value->tm_hour >= 0 && value->tm_hour <= 23 &&
                value->tm_min >= 0 && value->tm_min <= 59 &&
                value->tm_sec >= 0 && value->tm_sec <= 59;
    }
    return valid;
}

bool time_service_core_utc_to_epoch(const struct tm *value, int64_t *epoch)
{
    if (epoch == NULL || !time_service_core_tm_valid(value))
    {
        return false;
    }

    const int64_t days = _days_from_civil(value->tm_year + 1900,
                                          (unsigned)value->tm_mon + 1U,
                                          (unsigned)value->tm_mday);
    *epoch = days * INT64_C(86400) + (int64_t)value->tm_hour * 3600LL +
             (int64_t)value->tm_min * 60LL + (int64_t)value->tm_sec;
    return true;
}

bool time_service_core_epoch_to_utc(int64_t epoch, struct tm *value)
{
    if (value == NULL)
    {
        return false;
    }
    if (epoch < INT64_C(-62135596800) || epoch > INT64_C(253402300799))
    {
        return false;
    }

    int64_t days = epoch / INT64_C(86400);
    int64_t seconds = epoch % INT64_C(86400);
    if (seconds < 0)
    {
        seconds += INT64_C(86400);
        --days;
    }

    int year;
    unsigned month;
    unsigned day;
    _civil_from_days(days, &year, &month, &day);
    if (year < 1 || year > 9999)
    {
        return false;
    }

    memset(value, 0, sizeof(*value));
    value->tm_year = year - 1900;
    value->tm_mon = (int)month - 1;
    value->tm_mday = (int)day;
    value->tm_hour = (int)(seconds / 3600LL);
    value->tm_min = (int)((seconds % 3600LL) / 60LL);
    value->tm_sec = (int)(seconds % 60LL);

    int64_t weekday = (days + 4LL) % 7LL;
    if (weekday < 0)
    {
        weekday += 7LL;
    }
    value->tm_wday = (int)weekday;
    value->tm_yday = (int)(days - _days_from_civil(year, 1U, 1U));
    value->tm_isdst = 0;
    return true;
}
