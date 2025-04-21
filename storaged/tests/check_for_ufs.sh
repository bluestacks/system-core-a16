#!/system/bin/sh

# Copyright (C) 2025 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


# This script is used to determine if an Android device is using UFS or eMMC.
# We consider using UFS to be a "success" (exit code 0), and using eMMC or
# other unexpected issues to be a "failure" (non-zero exit code).

# There is no universal straight-forward way to determine UFS vs. eMMC, so
# we use educated guesses.  Our high level logic:
#
# Assume /dev/block/by-name/userdata is a symlink to /dev/block/USERDATA_BLOCK.
#
# - If USERDATA_BLOCK starts with "mmc", then this is eMMC.
# - If there is no "host0" found within /sys/devices, then this is eMMC.
# - If USERDATA_BLOCK is found within the "host0" directory in /sys/devices,
#   we are using UFS.
#
# If none of the above conditions hold, we're less confident.  If we couldn't
# find /dev/block/by-name/userdata but we did find "host0" in /sys/devices,
# we assume this is UFS.  If we did find USERDATA_BLOCK, and it's not in our
# "host0" in /sys/devices, then we guess non-UFS.


# Exit codes
readonly USING_UFS=0  # Must be 0 to indicate non-error
readonly USING_EMMC=1
readonly SETUP_ISSUE=2

# All of these shell commands are assumed to be on the device.
readonly REQUIRED_CMDS="find readlink sed"

readonly USERDATA_BY_NAME="/dev/block/by-name/userdata"

# Global variables (I know, but it's shell, so this is easiest).
userdata_block="UNSET"
host0_path="UNSET"


# Exit in failure if we're non-root or lack commands this script needs.
function check_setup() {
  if [ $(id -u) -ne 0 ]; then
    echo "ERROR: Need to run as root."
    exit ${SETUP_ISSUE}
  fi

  # We explicitly check for these commands, because if we're missing any,
  # this error message will be vastly easier to debug.
  which ${REQUIRED_CMDS} > /dev/null
  local which_result=$?

  if [ ${which_result} -ne 0 ]; then
    echo "ERROR: Missing at least one of the required binaries: ${REQUIRED_CMDS}"
    exit ${SETUP_ISSUE}
  fi
}


# Set the global variable userdata_block, or print a warning if we cannot.
function set_userdata_block {
  # Using global userdata_block

  local userdata_path=`readlink -e ${USERDATA_BY_NAME}`
  local readlink_result=$?

  if [ ${readlink_result} -eq 0 ]; then
    # Remove the "/dev/block/" part.
    userdata_block=`echo ${userdata_path} | sed 's#/dev/block/##'`
  else
    echo "Warning: Unable to find ${USERDATA_BY_NAME}"
    # We'll leave our userdata_block as "UNSET".
  fi
}


# If the userdata block starts with "mmc", it's eMMC.
function exit_if_userdata_block_is_emmc {
  # Uses global userdata_block

  case ${userdata_block} in
     mmc*)
       echo "userdata block is ${userdata_block}"
       echo "Using eMMC"
       exit ${USING_EMMC} ;;
  esac
}


# Set global host0_path, or exit in failure if we can't find it.
function set_host0_path_or_exit {
  # Using global host0_path

  # We quit after the first path we find.  We think this should be okay,
  # as further matches end up being subdirectories of the higher level
  # directory we care about (which will be found first).  This also makes
  # this run faster in the UFS case.
  host0_path=`find /sys/devices -name host0 -print -quit`

  if [ -z "${host0_path}" ]; then
    echo "Cannot find host0 in /sys/devices."
    echo "Using eMMC"
    exit ${USING_EMMC}
  fi
}


# Returning from this function means we think this is UFS.
function exit_if_not_ufs {
  # Using globals host0_path and userdata_block

  if [ "${userdata_block}" == "UNSET" ]; then
    # It's odd we couldn't find the userdata block.  But since we found host0,
    # we most likely are using UFS.  We've already printed out a warning about
    # not finding userdata.  So we'll just call this good.
    return 0
  fi

  # We use -print -quit to make this slightly faster.
  local find_output=`find ${host0_path} -name ${userdata_block} -print -quit`
  local find_result=$?

  # We check the exit code as well, to make sure find_output isn't an
  # (unexpected) error message of some sort.
  if [ ${find_result} -eq 0 ] && [ ! -z "${find_output}" ]; then
    # We've found our userdata within host0.  We're quite confident this is UFS.
    return 0
  fi

  echo "Warning: Could not find userdata ${userdata_block} within host0 path ${host0_path}"
  # While we found host0, which indicated UFS, it doesn't appear to be where
  # userdata is being used from.  We're honestly not sure if we're UFS or eMMC
  # here.  It seems worth failing here, to raise a flag on this (hopefully
  # unlikely) case.
  echo "Assuming this is eMMC"
  exit ${USING_EMMC}
}


check_setup

set_userdata_block
exit_if_userdata_block_is_emmc

set_host0_path_or_exit
exit_if_not_ufs

# We made it through all our checks.
echo "Using UFS"
exit ${USING_UFS}

