/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CRASH_REPORTER_CRASH_COLLECTOR_TEST_H_
#define CRASH_REPORTER_CRASH_COLLECTOR_TEST_H_

#include "crash_collector.h"

#include <map>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

class CrashCollectorMock : public CrashCollector {
 public:
  MOCK_METHOD0(SetUpDBus, void());
  MOCK_METHOD1(GetActiveUserSessions,
               bool(std::map<std::string, std::string> *sessions));
};

#endif  // CRASH_REPORTER_CRASH_COLLECTOR_TEST_H_
