/*
 * Copyright (c) 2015,2018 The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <errno.h>
#include <time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define LOG_TAG "QTI PowerHAL"
#include <utils/Log.h>
#include <hardware/hardware.h>
#include <hardware/power.h>

#include "utils.h"
#include "metadata-defs.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"

const int kMaxLaunchDuration = 5000; /* ms */
const int kMaxInteractiveDuration = 5000; /* ms */
const int kMinInteractiveDuration = 500; /* ms */
const int kMinFlingDuration = 1500; /* ms */

#define MIN_VAL(X,Y) ((X>Y)?(Y):(X))

static int saved_interactive_mode = -1;
static int display_hint_sent;
static int video_encode_hint_sent;
pthread_mutex_t camera_hint_mutex = PTHREAD_MUTEX_INITIALIZER;
static int camera_hint_ref_count;
static void process_video_encode_hint(void *metadata);
static int display_fd;
#define SYS_DISPLAY_PWR "/sys/kernel/hbtp/display_pwr"

static int process_activity_launch_hint(void *data)
{
    static int launch_handle = -1;
    static int launch_mode = 0;

    // release lock early if launch has finished
    if (!data) {
        if (CHECK_HANDLE(launch_handle)) {
            release_request(launch_handle);
            launch_handle = -1;
        }
        launch_mode = 0;
        return HINT_HANDLED;
    }

    if (!launch_mode) {
        launch_handle = perf_hint_enable_with_type(VENDOR_HINT_FIRST_LAUNCH_BOOST,
                kMaxLaunchDuration, LAUNCH_BOOST_V1);
        if (!CHECK_HANDLE(launch_handle)) {
            ALOGE("Failed to perform launch boost");
            return HINT_NONE;
        }
        launch_mode = 1;
    }
    return HINT_HANDLED;
}

static void process_interaction_hint(void *data)
{
    static struct timespec s_previous_boost_timespec;
    static int s_previous_duration = 0;

    struct timespec cur_boost_timespec;
    long long elapsed_time;
    int duration = kMinInteractiveDuration;

    if (data) {
        int input_duration = *((int*)data);
        if (input_duration > duration) {
            duration = (input_duration > kMaxInteractiveDuration) ?
                    kMaxInteractiveDuration : input_duration;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &cur_boost_timespec);

    elapsed_time = calc_timespan_us(s_previous_boost_timespec, cur_boost_timespec);
    // don't hint if previous hint's duration covers this hint's duration
    if ((s_previous_duration * 1000) > (elapsed_time + duration * 1000)) {
        return;
    }
    s_previous_boost_timespec = cur_boost_timespec;
    s_previous_duration = duration;

    if (duration >= kMinFlingDuration) {
        // Use launch boost resources for fling boost
        perf_hint_enable_with_type(VENDOR_HINT_FIRST_LAUNCH_BOOST, -1, LAUNCH_BOOST_V1);
    } else {
        perf_hint_enable_with_type(VENDOR_HINT_SCROLL_BOOST, duration, SCROLL_VERTICAL);
    }
}

int  power_hint_override(struct power_module *module, power_hint_t hint,
        void *data)
{

    switch(hint) {
        case POWER_HINT_VSYNC:
            break;
        case POWER_HINT_VIDEO_ENCODE:
        {
            process_video_encode_hint(data);
            return HINT_HANDLED;
        }
	case POWER_HINT_INTERACTION:
	{
            process_interaction_hint(data);
            return HINT_HANDLED;
	}
        case POWER_HINT_LAUNCH:
            return process_activity_launch_hint(data);
    }
    return HINT_NONE;
}

int  set_interactive_override(struct power_module *module, int on)
{
    char governor[80];
    char tmp_str[NODE_MAX];
    struct video_encode_metadata_t video_encode_metadata;
    int rc = 0;

    static const char *display_on = "1";
    static const char *display_off = "0";
    char err_buf[80];
    static int init_interactive_hint = 0;
    static int set_i_count = 0;

    ALOGI("Got set_interactive hint");

    if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return HINT_HANDLED;
                }
            }
        }
    }

    if (!on) {
        /* Display off. */
             if ((strncmp(governor, INTERACTIVE_GOVERNOR, strlen(INTERACTIVE_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
               int resource_values[] = {INT_OP_CLUSTER0_TIMER_RATE, BIG_LITTLE_TR_MS_50,
                                        INT_OP_CLUSTER1_TIMER_RATE, BIG_LITTLE_TR_MS_50,
                                        INT_OP_NOTIFY_ON_MIGRATE, 0x00};

               if (!display_hint_sent) {
                   perform_hint_action(DISPLAY_STATE_HINT_ID,
                   resource_values, sizeof(resource_values)/sizeof(resource_values[0]));
                  display_hint_sent = 1;
                }
             } /* Perf time rate set for CORE0,CORE4 8952 target*/

    } else {
        /* Display on. */
          if ((strncmp(governor, INTERACTIVE_GOVERNOR, strlen(INTERACTIVE_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {

             undo_hint_action(DISPLAY_STATE_HINT_ID);
             display_hint_sent = 0;
          }
   }
    saved_interactive_mode = !!on;

    set_i_count ++;
    ALOGI("Got set_interactive hint on= %d, count= %d\n", on, set_i_count);

    if (init_interactive_hint == 0)
    {
        //First time the display is turned off
        display_fd = TEMP_FAILURE_RETRY(open(SYS_DISPLAY_PWR, O_RDWR));
        if (display_fd < 0) {
            strerror_r(errno,err_buf,sizeof(err_buf));
            ALOGE("Error opening %s: %s\n", SYS_DISPLAY_PWR, err_buf);
            return HINT_HANDLED;
        }
        else
            init_interactive_hint = 1;
    }
    else
        if (!on ) {
            /* Display off. */
            rc = TEMP_FAILURE_RETRY(write(display_fd, display_off, strlen(display_off)));
            if (rc < 0) {
                strerror_r(errno,err_buf,sizeof(err_buf));
                ALOGE("Error writing %s to  %s: %s\n", display_off, SYS_DISPLAY_PWR, err_buf);
            }
        }
        else {
            /* Display on */
            rc = TEMP_FAILURE_RETRY(write(display_fd, display_on, strlen(display_on)));
            if (rc < 0) {
                strerror_r(errno,err_buf,sizeof(err_buf));
                ALOGE("Error writing %s to  %s: %s\n", display_on, SYS_DISPLAY_PWR, err_buf);
            }
        }

    return HINT_HANDLED;
}

/* Video Encode Hint */
static void process_video_encode_hint(void *metadata)
{
    char governor[80] = {0};
    int resource_values[20] = {0};
    int num_resources = 0;
    struct video_encode_metadata_t video_encode_metadata;

    ALOGI("Got process_video_encode_hint");

    if (get_scaling_governor_check_cores(governor,
        sizeof(governor),CPU0) == -1) {
            if (get_scaling_governor_check_cores(governor,
                sizeof(governor),CPU1) == -1) {
                    if (get_scaling_governor_check_cores(governor,
                        sizeof(governor),CPU2) == -1) {
                            if (get_scaling_governor_check_cores(governor,
                                sizeof(governor),CPU3) == -1) {
                                    ALOGE("Can't obtain scaling governor.");
                                    return;
                            }
                    }
            }
    }

    /* Initialize encode metadata struct fields. */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (metadata) {
        if (parse_video_encode_metadata((char *)metadata,
            &video_encode_metadata) == -1) {
            ALOGE("Error occurred while parsing metadata.");
            return;
        }
    } else {
        return;
    }

    if (video_encode_metadata.state == 1) {
        if((strncmp(governor, SCHEDUTIL_GOVERNOR,
            strlen(SCHEDUTIL_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(SCHEDUTIL_GOVERNOR))) {
             /* sample_ms = 10mS */
             int res[] = {0x41820000, 0xa,
                         };
             memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
             num_resources = sizeof(res)/sizeof(res[0]);
             pthread_mutex_lock(&camera_hint_mutex);
             camera_hint_ref_count++;
             if (camera_hint_ref_count == 1) {
                 if (!video_encode_hint_sent) {
                     perform_hint_action(video_encode_metadata.hint_id,
                     resource_values, num_resources);
                     video_encode_hint_sent = 1;
                }
            }
            pthread_mutex_unlock(&camera_hint_mutex);
        }
        else if ((strncmp(governor, INTERACTIVE_GOVERNOR,
            strlen(INTERACTIVE_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
           /* Sched_load and migration_notif*/
            int res[] = {INT_OP_CLUSTER0_USE_SCHED_LOAD,
                         0x1,
                         INT_OP_CLUSTER1_USE_SCHED_LOAD,
                         0x1,
                         INT_OP_CLUSTER0_USE_MIGRATION_NOTIF,
                         0x1,
                         INT_OP_CLUSTER1_USE_MIGRATION_NOTIF,
                         0x1,
                         INT_OP_CLUSTER0_TIMER_RATE,
                         BIG_LITTLE_TR_MS_40,
                         INT_OP_CLUSTER1_TIMER_RATE,
                         BIG_LITTLE_TR_MS_40
                         };
            memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
            num_resources = sizeof(res)/sizeof(res[0]);
            pthread_mutex_lock(&camera_hint_mutex);
            camera_hint_ref_count++;
            if (!video_encode_hint_sent) {
                perform_hint_action(video_encode_metadata.hint_id,
                resource_values,num_resources);
                video_encode_hint_sent = 1;
            }
            pthread_mutex_unlock(&camera_hint_mutex);
        }
    } else if (video_encode_metadata.state == 0) {
        if (((strncmp(governor, INTERACTIVE_GOVERNOR,
            strlen(INTERACTIVE_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) ||
            ((strncmp(governor, SCHEDUTIL_GOVERNOR,
            strlen(SCHEDUTIL_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(SCHEDUTIL_GOVERNOR)))) {
            pthread_mutex_lock(&camera_hint_mutex);
            camera_hint_ref_count--;
            if (!camera_hint_ref_count) {
                undo_hint_action(video_encode_metadata.hint_id);
                video_encode_hint_sent = 0;
            }
            pthread_mutex_unlock(&camera_hint_mutex);
            return ;
        }
    }
    return;
}
