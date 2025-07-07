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

#include <modprobe/utils.h>

#include <android-base/logging.h>
#include <android-base/strings.h>

namespace android {
namespace modprobe {

std::string CanonicalizeModulePath(const std::string& module_path) {
    auto start = module_path.find_last_of('/');
    if (start == std::string::npos) {
        start = 0;
    } else {
        start += 1;
    }
    auto end = module_path.size();
    if (android::base::EndsWith(module_path, ".ko")) {
        end -= 3;
    }
    if ((end - start) <= 1) {
        LOG(ERROR) << "malformed module name: " << module_path;
        return "";
    }
    std::string module_name = module_path.substr(start, end - start);
    // module names can have '-', but their file names will have '_'
    std::replace(module_name.begin(), module_name.end(), '-', '_');
    return module_name;
}

}  // namespace modprobe
}  // namespace android
