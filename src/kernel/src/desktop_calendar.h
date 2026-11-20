#pragma once
#include <stdint.h>

namespace brook {

inline constexpr char CalendarWeekdays[7][4] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
inline constexpr char CalendarMonths[12][10] = {"January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"};
inline constexpr uint16_t CalendarDaysBeforeMonth[13] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365
};

struct CalendarMonth {
    uint16_t year;
    uint8_t month; // 1–12, zero for unavailable
    constexpr bool operator==(const CalendarMonth& other) const {
        return year == other.year && month == other.month;
    }
};

struct CalendarDate {
    CalendarMonth month;
    uint8_t day;
};

struct CalendarClock {
    CalendarDate date;
    uint8_t hour, minute;
};

constexpr bool CalendarLeapYear(uint32_t year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

constexpr bool CalendarMonthValid(CalendarMonth month) {
    return month.year && month.year <= 9999 && month.month >= 1 && month.month <= 12;
}

constexpr unsigned CalendarDaysInMonth(CalendarMonth month) {
    if (!CalendarMonthValid(month)) return 0;
    if (month.month == 2 && CalendarLeapYear(month.year)) return 29;
    return CalendarDaysBeforeMonth[month.month] - CalendarDaysBeforeMonth[month.month - 1];
}

constexpr bool CalendarDateValid(CalendarDate date) {
    return date.day && date.day <= CalendarDaysInMonth(date.month);
}

// Absolute day 0 is Monday, 1 January 0001, in the proleptic Gregorian calendar.
constexpr int32_t CalendarDaysToYear(int32_t year) {
    const int32_t prior = year - 1;
    return prior * 365 + prior / 4 - prior / 100 + prior / 400;
}

constexpr int32_t CalendarAbsoluteDay(CalendarDate date) {
    const auto month = date.month;
    return CalendarDaysToYear(month.year) + CalendarDaysBeforeMonth[month.month - 1] +
           (month.month > 2 && CalendarLeapYear(month.year)) + date.day - 1;
}

constexpr unsigned CalendarWeekday(CalendarDate date) {
    return CalendarDateValid(date) ? unsigned(CalendarAbsoluteDay(date) % 7) : 7;
}

constexpr CalendarMonth CalendarStepMonth(CalendarMonth month, int direction) {
    if (!CalendarMonthValid(month)) return {};
    if (direction < 0) {
        if (month.month > 1) --month.month;
        else if (month.year > 1) { --month.year; month.month = 12; }
    } else if (direction > 0) {
        if (month.month < 12) ++month.month;
        else if (month.year < 9999) { ++month.year; month.month = 1; }
    }
    return month;
}

struct CalendarGrid {
    unsigned first, days, highlighted;
};

constexpr CalendarGrid CalendarMonthGrid(CalendarMonth month, CalendarDate today) {
    const auto days = CalendarDaysInMonth(month);
    if (!days) return {};
    return {CalendarWeekday({month, 1}), days,
            CalendarDateValid(today) && today.month == month ? today.day : 0u};
}

constexpr CalendarClock CalendarLocalClock(uint64_t utc, int32_t timezoneSeconds) {
    constexpr int32_t epoch = CalendarDaysToYear(1970);
    constexpr int32_t limit = CalendarDaysToYear(10000);
    // Validate before converting unsigned seconds or adding the signed offset.
    if (utc > uint64_t(limit - epoch) * 86400 + (uint64_t{1} << 31)) return {};
    const int64_t local = int64_t(utc) + timezoneSeconds;
    int64_t day = local / 86400 + epoch, time = local % 86400;
    if (time < 0) { --day; time += 86400; }
    if (day < 0 || day >= limit) return {};
    int32_t lo = 1, hi = 10000;
    while (hi - lo > 1) {
        const int32_t middle = (lo + hi) / 2;
        if (CalendarDaysToYear(middle) <= day) lo = middle;
        else hi = middle;
    }
    CalendarMonth month{uint16_t(lo), 1};
    day -= CalendarDaysToYear(lo);
    while (day >= int64_t(CalendarDaysInMonth(month))) {
        day -= CalendarDaysInMonth(month);
        ++month.month;
    }
    return {{month, uint8_t(day + 1)}, uint8_t(time / 3600), uint8_t(time / 60 % 60)};
}

} // namespace brook
