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

#include "src/cctz.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <vector>

namespace cctz {

namespace {

std::tm ToTM(const Breakdown& bd) {
  std::tm tm = {0};
  tm.tm_sec = bd.second;
  tm.tm_min = bd.minute;
  tm.tm_hour = bd.hour;
  tm.tm_mday = bd.day;
  tm.tm_mon = bd.month - 1;

  // Saturate tm.tm_year is cases of over/underflow.
  if (bd.year < std::numeric_limits<int>::min() + 1900) {
    tm.tm_year = std::numeric_limits<int>::min();
  } else if (bd.year - 1900 > std::numeric_limits<int>::max()) {
    tm.tm_year = std::numeric_limits<int>::max();
  } else {
    tm.tm_year = static_cast<int>(bd.year - 1900);
  }

  tm.tm_wday = bd.weekday % 7;
  tm.tm_yday = bd.yearday - 1;
  tm.tm_isdst = bd.is_dst ? 1 : 0;
  return tm;
}

const char kDigits[] = "0123456789";

// Formats a 64-bit integer in the given field width.  Note that it is up
// to the caller of Format64() [and Format02d()/FormatOffset()] to ensure
// that there is sufficient space before ep to hold the conversion.
char* Format64(char* ep, int width, int64_t v) {
  bool neg = false;
  if (v < 0) {
    --width;
    neg = true;
    if (v == INT64_MIN) {
      // Avoid negating INT64_MIN.
      int last_digit = -(v % 10);
      v /= 10;
      if (last_digit < 0) {
        ++v;
        last_digit += 10;
      }
      --width;
      *--ep = kDigits[last_digit];
    }
    v = -v;
  }
  do {
    --width;
    *--ep = kDigits[v % 10];
  } while (v /= 10);
  while (--width >= 0) *--ep = '0';  // zero pad
  if (neg) *--ep = '-';
  return ep;
}

// Formats [0 .. 99] as %02d.
char* Format02d(char* ep, int v) {
  *--ep = kDigits[v % 10];
  *--ep = kDigits[(v / 10) % 10];
  return ep;
}

// Formats a UTC offset, like +00:00.
char* FormatOffset(char* ep, int minutes, char sep) {
  char sign = '+';
  if (minutes < 0) {
    minutes = -minutes;
    sign = '-';
  }
  ep = Format02d(ep, minutes % 60);
  if (sep != '\0') *--ep = sep;
  ep = Format02d(ep, minutes / 60);
  *--ep = sign;
  return ep;
}

// Formats a std::tm using strftime(3).
void FormatTM(std::string* out, const std::string& fmt, const std::tm& tm) {
  // strftime(3) returns the number of characters placed in the output
  // array (which may be 0 characters).  It also returns 0 to indicate
  // an error, like the array wasn't large enough.  To accomodate this,
  // the following code grows the buffer size from 2x the format string
  // length up to 32x.
  for (int i = 2; i != 32; i *= 2) {
    size_t buf_size = fmt.size() * i;
    std::vector<char> buf(buf_size);
    if (size_t len = strftime(&buf[0], buf_size, fmt.c_str(), &tm)) {
      out->append(&buf[0], len);
      return;
    }
  }
}

// Used for %E#S specifiers and for data values in Parse().
template <typename T>
const char* ParseInt(const char* dp, int width, T min, T max, T* vp) {
  if (dp != nullptr) {
    const T kmin = std::numeric_limits<T>::min();
    bool erange = false;
    bool neg = false;
    T value = 0;
    if (*dp == '-') {
      neg = true;
      if (width <= 0 || --width != 0) {
        ++dp;
      } else {
        dp = nullptr;  // width was 1
      }
    }
    if (const char* const bp = dp) {
      while (const char* cp = strchr(kDigits, *dp)) {
        int d = static_cast<int>(cp - kDigits);
        if (d >= 10) break;
        if (value < kmin / 10) {
          erange = true;
          break;
        }
        value *= 10;
        if (value < kmin + d) {
          erange = true;
          break;
        }
        value -= d;
        dp += 1;
        if (width > 0 && --width == 0) break;
      }
      if (dp != bp && !erange && (neg || value != kmin)) {
        if (!neg || value != 0) {
          if (!neg) value = -value;  // make positive
          if (min <= value && value <= max) {
            *vp = value;
          } else {
            dp = nullptr;
          }
        } else {
          dp = nullptr;
        }
      } else {
        dp = nullptr;
      }
    }
  }
  return dp;
}

// The number of base-10 digits that can be represented by an int64_t.
const int kDigits10_64 = std::numeric_limits<int64_t>::digits10;

// 10^n for everything that can be represented by an int64_t.
const int64_t kExp10[kDigits10_64 + 1] = {
    1,
    10,
    100,
    1000,
    10000,
    100000,
    1000000,
    10000000,
    100000000,
    1000000000,
    10000000000,
    100000000000,
    1000000000000,
    10000000000000,
    100000000000000,
    1000000000000000,
    10000000000000000,
    100000000000000000,
    1000000000000000000,
};

}  // namespace

// Uses strftime(3) to format the given Time.  The following extended format
// specifiers are also supported:
//
//   - %Ez  - RFC3339-compatible numeric timezone (+hh:mm or -hh:mm)
//   - %E#S - Seconds with # digits of fractional precision
//   - %E*S - Seconds with full fractional precision (a literal '*')
//   - %E4Y - Four-character years (-999 ... -001, 0000, 0001 ... 9999)
//
// The standard specifiers from RFC3339_* (%Y, %m, %d, %H, %M, and %S) are
// handled internally for performance reasons.  strftime(3) is slow due to
// a POSIX requirement to respect changes to ${TZ}.
//
// The TZ/GNU %s extension is handled internally because strftime() has
// to use mktime() to generate it, and that assumes the local time zone.
//
// We also handle the %z and %Z specifiers to accommodate platforms that do
// not support the tm_gmtoff and tm_zone extensions to std::tm.
std::string Format(const std::string& format, const time_point& tp,
                   const TimeZone& tz) {
  std::string result;
  const Breakdown bd = BreakTime(tp, tz);
  const std::tm tm = ToTM(bd);

  // Scratch buffer for internal conversions.
  char buf[3 + kDigits10_64];  // enough for longest conversion
  char* const ep = buf + sizeof(buf);
  char* bp;  // works back from ep

  // Maintain three, disjoint subsequences that span format.
  //   [format.begin() ... pending) : already formatted into result
  //   [pending ... cur) : formatting pending, but no special cases
  //   [cur ... format.end()) : unexamined
  // Initially, everything is in the unexamined part.
  const char* pending = format.c_str();  // NUL terminated
  const char* cur = pending;
  const char* end = pending + format.length();

  while (cur != end) {  // while something is unexamined
    // Moves cur to the next percent sign.
    const char* start = cur;
    while (cur != end && *cur != '%') ++cur;

    // If the new pending text is all ordinary, copy it out.
    if (cur != start && pending == start) {
      result.append(pending, cur - pending);
      pending = start = cur;
    }

    // Span the sequential percent signs.
    const char* percent = cur;
    while (cur != end && *cur == '%') ++cur;

    // If the new pending text is all percents, copy out one
    // percent for every matched pair, then skip those pairs.
    if (cur != start && pending == start) {
      size_t escaped = (cur - pending) / 2;
      result.append(pending, escaped);
      pending += escaped * 2;
      // Also copy out a single trailing percent.
      if (pending != cur && cur == end) {
        result.push_back(*pending++);
      }
    }

    // Loop unless we have an unescaped percent.
    if (cur == end || (cur - percent) % 2 == 0) continue;

    // Simple specifiers that we handle ourselves.
    if (strchr("YmdeHMSzZs", *cur)) {
      if (cur - 1 != pending) {
        FormatTM(&result, std::string(pending, cur - 1), tm);
      }
      switch (*cur) {
        case 'Y':
          // This avoids the tm_year overflow problem for %Y, however
          // tm.tm_year will still be used by other specifiers like %D.
          bp = Format64(ep, 0, bd.year);
          result.append(bp, ep - bp);
          break;
        case 'm':
          bp = Format02d(ep, bd.month);
          result.append(bp, ep - bp);
          break;
        case 'd':
        case 'e':
          bp = Format02d(ep, bd.day);
          if (*cur == 'e' && *bp == '0') *bp = ' ';  // for Windows
          result.append(bp, ep - bp);
          break;
        case 'H':
          bp = Format02d(ep, bd.hour);
          result.append(bp, ep - bp);
          break;
        case 'M':
          bp = Format02d(ep, bd.minute);
          result.append(bp, ep - bp);
          break;
        case 'S':
          bp = Format02d(ep, bd.second);
          result.append(bp, ep - bp);
          break;
        case 'z':
          bp = FormatOffset(ep, bd.offset / 60, '\0');
          result.append(bp, ep - bp);
          break;
        case 'Z':
          result.append(bd.abbr);
          break;
        case 's':
          bp = Format64(
              ep, 0, std::chrono::duration_cast<std::chrono::duration<int64_t>>(
                         tp - std::chrono::system_clock::from_time_t(0))
                         .count());
          result.append(bp, ep - bp);
          break;
      }
      pending = ++cur;
      continue;
    }

    // Loop if there is no E modifier.
    if (*cur != 'E' || ++cur == end) continue;

    // Format our extensions.
    if (*cur == 'z') {
      // Formats %Ez.
      if (cur - 2 != pending) {
        FormatTM(&result, std::string(pending, cur - 2), tm);
      }
      bp = FormatOffset(ep, bd.offset / 60, ':');
      result.append(bp, ep - bp);
      pending = ++cur;
    } else if (*cur == '*' && cur + 1 != end && *(cur + 1) == 'S') {
      // Formats %E*S.
      if (cur - 2 != pending) {
        FormatTM(&result, std::string(pending, cur - 2), tm);
      }
      char* cp = ep;
      const int64_t nanoseconds = bd.subsecond.count();
      bp = Format64(cp, 9, nanoseconds);
      while (cp != bp && cp[-1] == '0') --cp;
      if (cp != bp) *--bp = '.';
      bp = Format02d(bp, bd.second);
      result.append(bp, cp - bp);
      pending = cur += 2;
    } else if (*cur == '4' && cur + 1 != end && *(cur + 1) == 'Y') {
      // Formats %E4Y.
      if (cur - 2 != pending) {
        FormatTM(&result, std::string(pending, cur - 2), tm);
      }
      bp = Format64(ep, 4, bd.year);
      result.append(bp, ep - bp);
      pending = cur += 2;
    } else if (std::isdigit(*cur)) {
      // Possibly found %E#S.
      int n = 0;
      if (const char* np = ParseInt(cur, 0, 0, 1024, &n)) {
        if (*np++ == 'S') {
          // Formats %E#S.
          if (cur - 2 != pending) {
            FormatTM(&result, std::string(pending, cur - 2), tm);
          }
          bp = ep;
          if (n > 0) {
            if (n > kDigits10_64) n = kDigits10_64;
            const int64_t nanoseconds = bd.subsecond.count();
            bp = Format64(bp, n, (n > 9) ? nanoseconds * kExp10[n - 9]
                                         : nanoseconds / kExp10[9 - n]);
            *--bp = '.';
          }
          bp = Format02d(bp, bd.second);
          result.append(bp, ep - bp);
          pending = cur = np;
        }
      }
    }
  }

  // Formats any remaining data.
  if (end != pending) {
    FormatTM(&result, std::string(pending, end), tm);
  }

  return result;
}

namespace {

const char* ParseOffset(const char* dp, char sep, int* offset) {
  if (dp != nullptr) {
    const char sign = *dp++;
    if (sign == '+' || sign == '-') {
      int hours = 0;
      const char* ap = ParseInt(dp, 2, 0, 23, &hours);
      if (ap != nullptr && ap - dp == 2) {
        dp = ap;
        if (sep != '\0' && *ap == sep) ++ap;
        int minutes = 0;
        const char* bp = ParseInt(ap, 2, 0, 59, &minutes);
        if (bp != nullptr && bp - ap == 2) dp = bp;
        *offset = (hours * 60 + minutes) * 60;
        if (sign == '-') *offset = -*offset;
      } else {
        dp = nullptr;
      }
    } else {
      dp = nullptr;
    }
  }
  return dp;
}

const char* ParseZone(const char* dp, std::string* zone) {
  zone->clear();
  if (dp != nullptr) {
    while (*dp != '\0' && !std::isspace(*dp)) zone->push_back(*dp++);
    if (zone->empty()) dp = nullptr;
  }
  return dp;
}

const char* ParseSubSeconds(const char* dp, duration* subseconds) {
  if (dp != nullptr) {
    if (*dp == '.') {
      int64_t v = 0;
      int64_t exp = 0;
      const char* const bp = ++dp;
      while (const char* cp = strchr(kDigits, *dp)) {
        int d = static_cast<int>(cp - kDigits);
        if (d >= 10) break;
        if (exp < 9) {
          exp += 1;
          v *= 10;
          v += d;
        }
        ++dp;
      }
      if (dp != bp) {
        v *= kExp10[9 - exp];
        *subseconds = std::chrono::nanoseconds(v);
      } else {
        dp = nullptr;
      }
    }
  }
  return dp;
}

// Parses a string into a std::tm using strptime(3).
const char* ParseTM(const char* dp, const char* fmt, std::tm* tm) {
  if (dp != nullptr) {
    dp = strptime(dp, fmt, tm);
  }
  return dp;
}

}  // namespace

// Uses strptime(3) to parse the given input.  Supports the same extended
// format specifiers as Format(), although %E#S and %E*S are treated
// identically.
//
// The standard specifiers from RFC3339_* (%Y, %m, %d, %H, %M, and %S) are
// handled internally so that we can normally avoid strptime() altogether
// (which is particularly helpful when the native implementation is broken).
//
// The TZ/GNU %s extension is handled internally because strptime() has to
// use localtime_r() to generate it, and that assumes the local time zone.
//
// We also handle the %z specifier to accommodate platforms that do not
// support the tm_gmtoff extension to std::tm.  %Z is parsed but ignored.
bool Parse(const std::string& format, const std::string& input,
           const TimeZone& tz, time_point* tpp) {
  // The unparsed input.
  const char* data = input.c_str();  // NUL terminated

  // Skips leading whitespace.
  while (std::isspace(*data)) ++data;

  const int kintmax = std::numeric_limits<int>::max();
  const int kintmin = std::numeric_limits<int>::min();

  // Sets default values for unspecified fields.
  std::tm tm = {0};
  tm.tm_year = 1970 - 1900;
  tm.tm_mon = 1 - 1;  // Jan
  tm.tm_mday = 1;
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  tm.tm_wday = 4;  // Thu
  tm.tm_yday = 0;
  tm.tm_isdst = 0;
  duration subseconds = duration::zero();
  int offset = kintmin;
  std::string zone = "UTC";

  const char* fmt = format.c_str();  // NUL terminated
  bool twelve_hour = false;
  bool afternoon = false;

  bool saw_precent_s = false;
  int64_t percent_s_time = 0;

  // Steps through format, one specifier at a time.
  while (data != nullptr && *fmt != '\0') {
    if (std::isspace(*fmt)) {
      while (std::isspace(*data)) ++data;
      while (std::isspace(*++fmt)) continue;
      continue;
    }

    if (*fmt != '%') {
      if (*data == *fmt) {
        ++data;
        ++fmt;
      } else {
        data = nullptr;
      }
      continue;
    }

    const char* percent = fmt;
    if (*++fmt == '\0') {
      data = nullptr;
      continue;
    }
    switch (*fmt++) {
      case 'Y':
        // We're more liberal than the 4-digit year typically handled by
        // strptime(), but we still need to store the result in an int,
        // and the intermediate value has a 1900 excess.
        data = ParseInt(data, 0, kintmin + 1900, kintmax, &tm.tm_year);
        if (data != nullptr) tm.tm_year -= 1900;
        continue;
      case 'm':
        data = ParseInt(data, 2, 1, 12, &tm.tm_mon);
        if (data != nullptr) tm.tm_mon -= 1;
        continue;
      case 'd':
        data = ParseInt(data, 2, 1, 31, &tm.tm_mday);
        continue;
      case 'H':
        data = ParseInt(data, 2, 0, 23, &tm.tm_hour);
        twelve_hour = false;
        continue;
      case 'M':
        data = ParseInt(data, 2, 0, 59, &tm.tm_min);
        continue;
      case 'S':
        data = ParseInt(data, 2, 0, 60, &tm.tm_sec);
        continue;
      case 'I':
      case 'r':  // probably uses %I
        twelve_hour = true;
        break;
      case 'R':  // uses %H
      case 'T':  // uses %H
      case 'c':  // probably uses %H
      case 'X':  // probably uses %H
        twelve_hour = false;
        break;
      case 'z':
        data = ParseOffset(data, '\0', &offset);
        continue;
      case 'Z':  // ignored; zone abbreviations are ambiguous
        data = ParseZone(data, &zone);
        continue;
      case 's':
        data = ParseInt(data, 0, INT64_MIN, INT64_MAX, &percent_s_time);
        if (data != nullptr) saw_precent_s = true;
        continue;
      case 'E':
        if (*fmt == 'z') {
          if (data != nullptr && *data == 'Z') {  // Zulu
            offset = 0;
            data += 1;
          } else {
            data = ParseOffset(data, ':', &offset);
          }
          fmt += 1;
          continue;
        }
        if (*fmt == '*' && *(fmt + 1) == 'S') {
          data = ParseInt(data, 2, 0, 60, &tm.tm_sec);
          data = ParseSubSeconds(data, &subseconds);
          fmt += 2;
          continue;
        }
        if (*fmt == '4' && *(fmt + 1) == 'Y') {
          const char* bp = data;
          data = ParseInt(data, 4, -999, 9999, &tm.tm_year);
          if (data != nullptr) {
            if (data - bp == 4) {
              tm.tm_year -= 1900;
            } else {
              data = nullptr;  // stopped too soon
            }
          }
          fmt += 2;
          continue;
        }
        if (std::isdigit(*fmt)) {
          int n = 0;
          if (const char* np = ParseInt(fmt, 0, 0, 1024, &n)) {
            if (*np++ == 'S') {
              data = ParseInt(data, 2, 0, 60, &tm.tm_sec);
              if (n > 0) {  // n is otherwise ignored
                data = ParseSubSeconds(data, &subseconds);
              }
              fmt = np;
              continue;
            }
          }
        }
        if (*fmt == 'c') twelve_hour = false;  // probably uses %H
        if (*fmt == 'X') twelve_hour = false;  // probably uses %H
        if (*fmt != '\0') ++fmt;
        break;
      case 'O':
        if (*fmt == 'H') twelve_hour = false;
        if (*fmt == 'I') twelve_hour = true;
        if (*fmt != '\0') ++fmt;
        break;
    }

    // Parses the current specifier.
    const char* orig_data = data;
    std::string spec(percent, fmt - percent);
    data = ParseTM(data, spec.c_str(), &tm);

    // If we successfully parsed %p we need to remember whether the result
    // was AM or PM so that we can adjust tm_hour before ConvertDateTime().
    // So reparse the input with a known AM hour, and check if it is shifted
    // to a PM hour.
    if (spec == "%p" && data != nullptr) {
      std::string test_input = "1" + std::string(orig_data, data - orig_data);
      const char* test_data = test_input.c_str();
      std::tm tmp = {0};
      ParseTM(test_data, "%I%p", &tmp);
      afternoon = (tmp.tm_hour == 13);
    }
  }

  // Adjust a 12-hour tm_hour value if it should be in the afternoon.
  if (twelve_hour && afternoon && tm.tm_hour < 12) {
    tm.tm_hour += 12;
  }

  if (data == nullptr) return false;

  // Skip any remaining whitespace.
  while (std::isspace(*data)) ++data;

  // Parse() must consume the entire input string.
  if (*data != '\0') return false;

  // If we saw %s then we ignore anything else and return that time.
  if (saw_precent_s) {
    *tpp = time_point(std::chrono::duration<int64_t>(percent_s_time));
    return true;
  }

  // If we saw %z or %Ez then we want to interpret the parsed fields in
  // UTC and then shift by that offset.  Otherwise we want to interpret
  // the fields directly in the passed TimeZone.
  TimeZone ptz = tz;
  if (offset != kintmin) {
    ptz = UTCTimeZone();  // Override tz.  Offset applied later.
  } else {
    offset = 0;  // No offset from passed tz.
  }

  // Allows a leap second of 60 to normalize forward to the following ":00".
  if (tm.tm_sec == 60) {
    tm.tm_sec -= 1;
    offset -= 1;
    subseconds = duration::zero();
  }

  int64_t year = tm.tm_year;
  if (year > INT64_MAX - 1900) {
    year = INT64_MAX;
  } else {
    year += 1900;
  }
  const TimeInfo ti = MakeTimeInfo(year, tm.tm_mon + 1, tm.tm_mday,
                                   tm.tm_hour, tm.tm_min, tm.tm_sec, ptz);

  // Parse() fails if any normalization was done.  That is,
  // parsing "Sep 31" will not produce the equivalent of "Oct 1".
  if (ti.normalized) return false;

  *tpp = ti.pre - std::chrono::seconds(offset) + subseconds;
  return true;
}

}  // namespace cctz
