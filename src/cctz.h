// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
//     Unless required by applicable law or agreed to in writing, software
//     distributed under the License is distributed on an "AS IS" BASIS,
//     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
//     implied.
//     See the License for the specific language governing permissions and
//     limitations under the License.

// CCTZ is a library for translating between absolute times (cctz::time_point)
// and civil times (year, month, day, hour, minute, second) using the rules
// defined by a time zone (cctz::TimeZone).
//
// Example:
//
//   cctz::TimeZone lax;
//   if (cctz::LoadTimeZone("America/Los_Angeles", &lax)) {
//     cctz::time_point tp = cctz::MakeTime(2015, 1, 2, 3, 4, 5, lax);
//     cctz::Breakdown bd = cctz::BreakTime(tp, lax);
//     // bd.year == 2015
//     // bd.month == 1
//     // ...
//     std:string s = cctz::Format("%Y-%m-%d %H:%M:%S %Ez", tp, lax);
//     // s == "2015-01-02 03:04:05 -08:00"
//   }

#ifndef CCTZ_H_
#define CCTZ_H_

#include <chrono>
#include <cstdint>
#include <string>

namespace cctz {

// Defines a time_point of the std::chrono::system_clock with a 128-bit
// representation (but still with nanosecond resolution) to extend the range
// beyond the +-292 years provided by the predefined types.
using duration = std::chrono::duration<__int128, std::nano>;
using time_point = std::chrono::time_point<std::chrono::system_clock, duration>;

// cctz::TimeZone is an opaque, small, value-type class representing a
// geo-political region within which particular rules are used for mapping
// between absolute and civil times. TimeZones are named using the TZ
// identifiers from the IANA Time Zone Database, such as "America/Los_Angeles"
// or "Australia/Sydney".  TimeZones are created from factory functions such
// as LoadTimeZone().  Note: strings like "PST" and "EDT" are not valid TZ
// identifiers.
//
// Examples:
//   cctz::TimeZone utc = cctz::UTCTimeZone();
//   cctz::TimeZone loc = cctz::LocalTimeZone();
//   cctz::TimeZone lax;
//   if (!cctz::LoadTimeZone("America/Los_Angeles", &lax)) { ... }
//
// See also:
// - http://www.iana.org/time-zones
// - http://en.wikipedia.org/wiki/Zoneinfo
class TimeZone {
 public:
  // A value type.
  TimeZone() = default;  // Equivalent to UTC
  TimeZone(const TimeZone&) = default;
  TimeZone& operator=(const TimeZone&) = default;

  class Impl;

 private:
  explicit TimeZone(const Impl* impl) : impl_(impl) {}
  const Impl* impl_ = nullptr;
};

// Loads the named zone. May perform I/O on the initial load of the named
// zone. If the name is invalid, or some other kind of error occurs, returns
// false and "*tz" is set to the UTC time zone.
bool LoadTimeZone(const std::string& name, TimeZone* tz);
// Convenience method returning the UTC time zone.
TimeZone UTCTimeZone();
// Convenience method returning the local time zone, or UTC if there is no
// configured local zone.
TimeZone LocalTimeZone();

// The calendar and wall-clock (a.k.a. "civil time") components of a
// time_point in a certain TimeZone. A better std::tm. This struct is not
// intended to represent an instant in time. So, rather than passing a
// Breakdown to a function, pass a time_point and a TimeZone.
struct Breakdown {
  int64_t year;        // year (e.g., 2013)
  int month;           // month of year [1:12]
  int day;             // day of month [1:31]
  int hour;            // hour of day [0:23]
  int minute;          // minute of hour [0:59]
  int second;          // second of minute [0:59]
  duration subsecond;  // [0s:1s)
  int weekday;         // 1==Mon, ..., 7=Sun
  int yearday;         // day of year [1:366]
  int offset;          // seconds east of UTC
  bool is_dst;         // is offset non-standard?
  std::string abbr;    // time-zone abbreviation (e.g., "PST")
};

// Returns the civil time components (and some other data) for the given
// absolute time in the given time zone.
Breakdown BreakTime(const time_point& tp, const TimeZone& tz);

// Returns the cctz::time_point corresponding to the given civil time fields
// in the given TimeZone after normalizing the fields. If the given civil time
// refers to a time that is either skipped or repeated (see the TimeInfo doc),
// then the as-if rule is followed and the time_point according to the
// pre-transition offset is returned.
time_point MakeTime(int64_t year, int mon, int day, int hour, int min, int sec,
                    const TimeZone& tz);

// A TimeInfo represents the conversion of year, month, day, hour, minute, and
// second values, in a particular cctz::TimeZone, to a time instant, as
// returned by MakeTimeInfo(). (Subseconds must be handled separately.)
//
// It is possible, though, for a caller to try to convert values that do not
// represent an actual or unique instant in time (due to a shift in UTC offset
// in the TimeZone, which results in a discontinuity in the civil-time
// components). For example, a daylight-saving-time transition skips or
// repeats civil times---in the United States, March 13, 2011 02:15 never
// occurred, while November 6, 2011 01:15 occurred twice---so requests for
// such times are not well-defined.
//
// To account for these possibilities, TimeInfo is richer than just a single
// time_point. When the civil time is skipped or repeated, MakeTimeInfo()
// returns times calculated using the pre-transition and post-transition UTC
// offsets, plus the transition time itself.
//
// Examples:
//   cctz::TimeZone lax;
//   if (!cctz::LoadTimeZone("America/Los_Angeles", &lax)) { ... }
//
//   // A unique civil time.
//   cctz::TimeInfo jan01 = cctz::MakeTimeInfo(2011, 1, 1, 0, 0, 0, lax);
//   // jan01.kind == TimeInfo::Kind::UNIQUE
//   // jan01.pre    is 2011/01/01 00:00:00 -0800
//   // jan01.trans  is 2011/01/01 00:00:00 -0800
//   // jan01.post   is 2011/01/01 00:00:00 -0800
//
//   // A Spring DST transition, when there is a gap in civil time.
//   cctz::TimeInfo mar13 = cctz::MakeTimeInfo(2011, 3, 13, 2, 15, 0, lax);
//   // mar13.kind == TimeInfo::Kind::SKIPPED
//   // mar13.pre   is 2011/03/13 03:15:00 -0700
//   // mar13.trans is 2011/03/13 03:00:00 -0700
//   // mar13.post  is 2011/03/13 01:15:00 -0800
//
//   // A Fall DST transition, when civil times are repeated.
//   cctz::TimeInfo nov06 = cctz::MakeTimeInfo(2011, 11, 6, 1, 15, 0, lax);
//   // nov06.kind == TimeInfo::Kind::REPEATED
//   // nov06.pre   is 2011/11/06 01:15:00 -0700
//   // nov06.trans is 2011/11/06 01:00:00 -0800
//   // nov06.post  is 2011/11/06 01:15:00 -0800
//
// The input month, day, hour, minute, and second values can also be
// outside of their valid ranges, in which case they will be "normalized"
// during the conversion.
//
// Example:
//   // "October 32" normalizes to "November 1".
//   cctz::TimeZone tz = ...
//   cctz::TimeInfo ti = cctz::MakeTimeInfo(2013, 10, 32, 8, 30, 0, tz);
//   // ti.kind == TimeInfo::Kind::UNIQUE && ti.normalized == true
//   // ti.pre.In(tz).month == 11 && ti.pre.In(tz).day == 1
struct TimeInfo {
  enum class Kind {
    UNIQUE,    // the civil time was singular (pre == trans == post)
    SKIPPED,   // the civil time did not exist
    REPEATED,  // the civil time was ambiguous
  } kind;
  time_point pre;  // Uses pre-transition offset
  time_point trans;
  time_point post;  // Uses post-transition offset
  bool normalized;
};

// Returns a TimeInfo corresponding to the given civil time fields in
// the given TimeZone after normalizing the fields. NOTE: Prefer calling
// the MakeTime() function unless you know that the default time_point
// that it returns is not what you want.
TimeInfo MakeTimeInfo(int64_t year, int mon, int day, int hour,
                      int min, int sec, const TimeZone& tz);

// Formats the given cctz::time_point in the given cctz::TimeZone according to
// the provided format string. Uses strftime()-like formatting options, with
// the following extensions:
//
//   - %Ez  - RFC3339-compatible numeric time zone (+hh:mm or -hh:mm)
//   - %E#S - Seconds with # digits of fractional precision
//   - %E*S - Seconds with full fractional precision (a literal '*')
//   - %E4Y - Four-character years (-999 ... -001, 0000, 0001 ... 9999)
//
// Note that %Y produces as many characters as it takes to fully render the
// year.  A year outside of [-999:9999] when formatted with %E4Y will produce
// more than four characters, just like %Y.
//
// Format strings should include %Ez so that the result uniquely identifies
// a time instant.
//
// Example:
//   cctz::TimeZone lax;
//   if (!cctz::LoadTimeZone("America/Los_Angeles", &lax)) { ... }
//   cctz::time_point tp = cctz::MakeTime(2013, 1, 2, 3, 4, 5, lax);
//
//   std::string f = cctz::Format("%H:%M:%S", tp, lax);  // "03:04:05"
//   f = cctz::Format("%H:%M:%E3S", tp, lax);  // "03:04:05.000"
std::string Format(const std::string& format, const time_point& tp,
                   const TimeZone& tz);

// Parses an input string according to the provided format string and returns
// the corresponding cctz::time_point. Uses strftime()-like formatting
// options, with the same extensions as cctz::Format().
//
// %Y consumes as many numeric characters as it can, so the matching data
// should always be terminated with a non-numeric. %E4Y always consumes
// exactly four characters, including any sign.
//
// Unspecified fields are taken from the default date and time of ...
//
//   "1970-01-01 00:00:00.0 +0000"
//
// For example, parsing a string of "15:45" (%H:%M) will return a
// cctz::time_point that represents "1970-01-01 15:45:00.0 +0000".
// Note: Since Parse() returns time instants, it makes the most sense to parse
// fully-specified date/time strings that include a UTC offset (%z/%Ez).
//
// Note also that Parse() only heeds the fields year, month, day, hour,
// minute, (fractional) second, and UTC offset.  Other fields, like
// weekday (%a or %A), while parsed for syntactic validity, are ignored
// in the conversion.
//
// Date and time fields that are out-of-range will be treated as errors rather
// than normalizing them like cctz::MakeTime() does. For example, it is an
// error to parse the date "Oct 32, 2013" because 32 is out of range.
//
// A leap second of ":60" is normalized to ":00" of the following minute with
// fractional seconds discarded. The following table shows how the given
// seconds and subseconds will be parsed:
//
//   "59.x" -> 59.x  // exact
//   "60.x" -> 00.0  // normalized
//   "00.x" -> 00.x  // exact
//
// Errors are indicated by returning false.
bool Parse(const std::string& format, const std::string& input,
           const TimeZone& tz, time_point* tpp);

}  // namespace cctz

#endif  // CCTZ_H_
