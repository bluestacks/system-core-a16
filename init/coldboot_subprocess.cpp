/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "coldboot.h"

#include <android-base/chrono_utils.h>
#include <android-base/logging.h>
#include <selinux/android.h>
#include <selinux/selinux.h>
#include <sys/wait.h>

namespace android {
namespace init {

void ColdBoot::UeventHandlerMain(unsigned int process_num, unsigned int total_processes) {
    for (unsigned int i = process_num; i < uevent_queue_.size(); i += total_processes) {
        auto& uevent = uevent_queue_[i];

        for (auto& uevent_handler : uevent_handlers_) {
            uevent_handler->HandleUevent(uevent);
        }
    }
}

void ColdBoot::RestoreConHandler(unsigned int process_num, unsigned int total_processes) {
    android::base::Timer t_process;

    for (unsigned int i = process_num; i < restorecon_queue_.size(); i += total_processes) {
        android::base::Timer t;
        auto& dir = restorecon_queue_[i];

        selinux_android_restorecon(dir.c_str(), SELINUX_ANDROID_RESTORECON_RECURSE);

        // Mark a dir restorecon operation for 50ms,
        // Maybe you can add this dir to the ueventd.rc script to parallel processing
        if (t.duration() > 50ms) {
            LOG(INFO) << "took " << t.duration().count() << "ms restorecon '" << dir.c_str()
                      << "' on process '" << process_num << "'";
        }
    }

    // Calculate process restorecon time
    LOG(VERBOSE) << "took " << t_process.duration().count() << "ms on process '" << process_num
                 << "'";
}

void ColdBoot::ForkSubProcesses() {
    for (unsigned int i = 0; i < num_handler_subprocesses_; ++i) {
        auto pid = fork();
        if (pid < 0) {
            PLOG(FATAL) << "fork() failed!";
        }

        if (pid == 0) {
            UeventHandlerMain(i, num_handler_subprocesses_);
            if (enable_parallel_restorecon_) {
                RestoreConHandler(i, num_handler_subprocesses_);
            }
            _exit(EXIT_SUCCESS);
        }

        subprocess_pids_.emplace(pid);
    }
}

void ColdBoot::WaitForSubProcesses() {
    // Treat subprocesses that crash or get stuck the same as if ueventd itself has crashed or gets
    // stuck.
    //
    // When a subprocess crashes, we fatally abort from ueventd.  init will restart ueventd when
    // init reaps it, and the cold boot process will start again.  If this continues to fail, then
    // since ueventd is marked as a critical service, init will reboot to bootloader.
    //
    // When a subprocess gets stuck, keep ueventd spinning waiting for it.  init has a timeout for
    // cold boot and will reboot to the bootloader if ueventd does not complete in time.
    while (!subprocess_pids_.empty()) {
        int status;
        pid_t pid = TEMP_FAILURE_RETRY(waitpid(-1, &status, 0));
        if (pid == -1) {
            PLOG(ERROR) << "waitpid() failed";
            continue;
        }

        auto it = std::find(subprocess_pids_.begin(), subprocess_pids_.end(), pid);
        if (it == subprocess_pids_.end()) continue;

        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == EXIT_SUCCESS) {
                subprocess_pids_.erase(it);
            } else {
                LOG(FATAL) << "subprocess exited with status " << WEXITSTATUS(status);
            }
        } else if (WIFSIGNALED(status)) {
            LOG(FATAL) << "subprocess killed by signal " << WTERMSIG(status);
        }
    }
}

}  // namespace init
}  // namespace android
