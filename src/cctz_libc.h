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

#ifndef CCTZ_LIBC_H_
#define CCTZ_LIBC_H_

#include <string>

#include "src/cctz_if.h"

namespace cctz {

// A time zone backed by gmtime_r(3), localtime_r(3), and mktime(3), and
// which therefore only supports "localtime" and fixed offsets from UTC.
class TimeZoneLibC : public TimeZoneIf {
 public:
  explicit TimeZoneLibC(const std::string& name);

  // TimeZoneIf implementations.
  Breakdown BreakTime(const time_point& tp) const override;
  TimeInfo MakeTimeInfo(int64_t year, int mon, int day,
                        int hour, int min, int sec) const override;

 private:
  bool local_;        // localtime or UTC
  int offset_;        // UTC offset when !local_
  std::string abbr_;  // abbreviation when !local_
};

}  // namespace cctz

#endif  // CCTZ_LIBC_H_
