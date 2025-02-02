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

#include <iostream>
#include <string>

#include "src/cctz.h"

int main() {
  const std::string civil_string = "2015-09-22 09:35:00";

  cctz::TimeZone lax;
  LoadTimeZone("America/Los_Angeles", &lax);
  cctz::time_point tp;
  const bool ok = cctz::Parse("%Y-%m-%d %H:%M:%S", civil_string, lax, &tp);
  if (!ok) return -1;

  const auto now = std::chrono::system_clock::now();
  const std::string s = now > tp ? "running long!" : "on time!";
  std::cout << "Talk " << s << "\n";
}
