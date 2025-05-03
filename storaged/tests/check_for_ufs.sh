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
readonly INTERNAL_ERROR=3

# All of these shell commands are assumed to be on the device.
readonly REQUIRED_CMDS="find readlink sed"

readonly USERDATA_BY_NAME="/dev/block/by-name/userdata"

# Global variables (I know, but it's shell, so this is easiest).
userdata_block="UNSET"
host0_path="UNSET"


# The output of this script will be used by automated testing, and analyzed
# at scale.  As such, we want to normalize the output, and try to just have
# a single line to analyze.
function exit_script() {
  exit_code=$1
  message="$2"

  prefix=""
  case ${exit_code} in
    ${USING_UFS}) prefix="UFS Detected";;
    ${USING_EMMC}) prefix="eMMC Detected";;
    ${SETUP_ISSUE}) prefix="ERROR";;
    ${INTERNAL_ERROR}) prefix="INTERNAL ERROR";;
    *)
      prefix="UNEXPECTED EXIT CODE (${exit_code})"
      exit_code=${INTERNAL_ERROR}
      ;;
  esac

  echo "${prefix}: ${message}"
  exit ${exit_code}
}

# Exit in failure if we're non-root or lack commands this script needs.
function check_setup() {
  if [ $(id -u) -ne 0 ]; then
    msg="Need to run as root."
    exit_script ${SETUP_ISSUE} "${msg}"
  fi

  # We explicitly check for these commands, because if we're missing any,
  # this error message will be vastly easier to debug.
  which ${REQUIRED_CMDS} > /dev/null
  local which_result=$?

  if [ ${which_result} -ne 0 ]; then
    msg="Missing at least one of the required binaries: ${REQUIRED_CMDS}"
    exit_script ${SETUP_ISSUE} "${msg}"
  fi
}


# Set the global variable userdata_block, or print a warning if we cannot.
function set_userdata_block {
  # Using global userdata_block

  local userdata_path=`readlink -e ${USERDATA_BY_NAME}`
  local readlink_result=$?

  # If we fail this 'if', we'll leave our userdata_block as "UNSET".
  if [ ${readlink_result} -eq 0 ]; then
    # Remove the "/dev/block/" part.
    userdata_block=`echo ${userdata_path} | sed 's#/dev/block/##'`
  fi
}


# If the userdata block starts with "mmc", it's eMMC.
function exit_if_userdata_block_is_emmc {
  # Uses global userdata_block

  case ${userdata_block} in
     mmc*)
       msg="userdata block is ${userdata_block}"
       exit_script ${USING_EMMC} "${msg}"
       ;;
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
    msg="Cannot find host0 in /sys/devices."
    exit_script ${USING_EMMC} "${msg}"
  fi
}


function check_for_userdata_block_in_host0_path {
  # Using globals host0_path and userdata_block

  if [ "${userdata_block}" == "UNSET" ]; then
    # It's odd we couldn't find the userdata block.  But since we found host0,
    # we most likely are using UFS.
    msg="Could not find ${USERDATA_BY_NAME}, but found host0"
    exit_script ${USING_UFS} "${msg}"
  fi

  # We use -print -quit to make this slightly faster.
  local find_output=`find ${host0_path} -name ${userdata_block} -print -quit`
  local find_result=$?

  # We check the exit code as well, to make sure find_output isn't an
  # (unexpected) error message of some sort.
  if [ ${find_result} -eq 0 ] && [ ! -z "${find_output}" ]; then
    # We've found our userdata within host0.  We're quite confident this is UFS.
    msg="Found ${userdata_block} within host0"
    exit_script ${USING_UFS} "${msg}"
  fi

  # While we found host0, which indicated UFS, it doesn't appear to be where
  # userdata is being used from.  We're honestly not sure if we're UFS or eMMC
  # here.  It seems worth failing here, to raise a flag on this (hopefully
  # unlikely) case.
  msg="Couldn't find userdata ${userdata_block} within host0 path ${host0_path}"
  exit_script ${USING_EMMC} "${msg}"
}


check_setup

set_userdata_block
exit_if_userdata_block_is_emmc

set_host0_path_or_exit

# This function will exit, concluding either eMMC or UFS.
check_for_userdata_block_in_host0_path

exit_script ${INTERNAL_ERROR} "Unexpectedly at the end of the script file"
