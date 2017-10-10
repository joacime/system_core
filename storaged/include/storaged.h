/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef _STORAGED_H_
#define _STORAGED_H_

#include <semaphore.h>
#include <stdint.h>
#include <time.h>

#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include <batteryservice/IBatteryPropertiesListener.h>
#include <batteryservice/IBatteryPropertiesRegistrar.h>

#define FRIEND_TEST(test_case_name, test_name) \
friend class test_case_name##_##test_name##_Test

#define ARRAY_SIZE(x)   (sizeof(x) / sizeof((x)[0]))

#define SECTOR_SIZE ( 512 )
#define SEC_TO_MSEC ( 1000 )
#define MSEC_TO_USEC ( 1000 )
#define USEC_TO_NSEC ( 1000 )
#define SEC_TO_USEC ( 1000000 )
#define HOUR_TO_SEC ( 3600 )
#define DAY_TO_SEC ( 3600 * 24 )
#define WEEK_TO_DAYS ( 7 )
#define YEAR_TO_WEEKS ( 52 )

#include "storaged_diskstats.h"
#include "storaged_info.h"
#include "storaged_uid_monitor.h"
#include "storaged.pb.h"

using namespace std;
using namespace android;

// Periodic chores intervals in seconds
#define DEFAULT_PERIODIC_CHORES_INTERVAL_UNIT ( 60 )
#define DEFAULT_PERIODIC_CHORES_INTERVAL_DISK_STATS_PUBLISH ( 3600 )
#define DEFAULT_PERIODIC_CHORES_INTERVAL_UID_IO ( 3600 )
#define DEFAULT_PERIODIC_CHORES_INTERVAL_UID_IO_LIMIT (300)
#define DEFAULT_PERIODIC_CHORES_INTERVAL_FLUSH_PROTO (3600)

// UID IO threshold in bytes
#define DEFAULT_PERIODIC_CHORES_UID_IO_THRESHOLD ( 1024 * 1024 * 1024ULL )

struct storaged_config {
    int periodic_chores_interval_unit;
    int periodic_chores_interval_disk_stats_publish;
    int periodic_chores_interval_uid_io;
    int periodic_chores_interval_flush_proto;
    int event_time_check_usec;  // check how much cputime spent in event loop
};

class storaged_t : public BnBatteryPropertiesListener,
                   public IBinder::DeathRecipient {
private:
    time_t mTimer;
    storaged_config mConfig;
    disk_stats_monitor mDsm;
    uid_monitor mUidm;
    time_t mStarttime;
    sp<IBatteryPropertiesRegistrar> battery_properties;
    unique_ptr<storage_info_t> storage_info;
    static const uint32_t crc_init;
    static const string proto_file;
    storaged_proto::StoragedProto proto;
public:
    storaged_t(void);
    ~storaged_t() {}
    void event(void);
    void event_checked(void);
    void pause(void) {
        sleep(mConfig.periodic_chores_interval_unit);
    }

    time_t get_starttime(void) {
        return mStarttime;
    }

    unordered_map<uint32_t, struct uid_info> get_uids(void) {
        return mUidm.get_uid_io_stats();
    }

    vector<vector<uint32_t>> get_perf_history(void) {
        return storage_info->get_perf_history();
    }

    map<uint64_t, struct uid_records> get_uid_records(
            double hours, uint64_t threshold, bool force_report) {
        return mUidm.dump(hours, threshold, force_report,
                          proto.mutable_uid_io_usage());
    }
    void update_uid_io_interval(int interval) {
        if (interval >= DEFAULT_PERIODIC_CHORES_INTERVAL_UID_IO_LIMIT) {
            mConfig.periodic_chores_interval_uid_io = interval;
        }
    }

    void init_battery_service();
    virtual void batteryPropertiesChanged(struct BatteryProperties props);
    void binderDied(const wp<IBinder>& who);

    void report_storage_info();

    void load_proto();
    void flush_proto();
};

// Eventlog tag
// The content must match the definition in EventLogTags.logtags
#define EVENTLOGTAG_DISKSTATS ( 2732 )
#define EVENTLOGTAG_EMMCINFO ( 2733 )
#define EVENTLOGTAG_UID_IO_ALERT ( 2734 )

#endif /* _STORAGED_H_ */
