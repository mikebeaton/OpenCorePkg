#!/bin/sh

#
# Copyright © 2019-2022 Rodion Shingarev, PMheart, vit9696, mikebeaton.
#
# Includes logic to install and run this script as one or more of
# agent, daemon or logout hook. Can also be run directly for testing,
# use CTRL+C for script termination.
#
# Currently installs just as launch daemon, which is the only one that
# on shutdown can access NVRAM after macOS installer vars are set.
#

LOG=/dev/stdout

usage() {
  echo "Usage: ${SELFNAME} [install|uninstall|status] [logout|agent|daemon]"
  echo "  - Use [install|uninstall|status] with no type to use"
  echo "    recommended settings (i.e. daemon)."
  echo "  - If called with no params runs once as logout hook, this"
  echo "    saves current nvram once or reports any issues."
  echo ""
}

doLog() {
  echo "$(date) (${PREFIX}) ${1}" >> "${LOG}"
}

abort() {
  doLog "Fatal error: ${1}"
  exit 1
}

if [ ! -x /usr/bin/dirname   ] ||
   [ ! -x /usr/bin/basename  ] ||
   [ ! -x /usr/bin/wc        ] ||
   [ ! -x /usr/sbin/diskutil ] ||
   [ ! -x /usr/sbin/nvram    ] ; then
  abort "Unix environment is broken!"
fi

OCNVRAMGUID="4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102"

# real OC NVRAM var
BOOT_PATH="${OCNVRAMGUID}:boot-path"

# temp storage name for this hook
BOOT_NODE="${OCNVRAMGUID}:boot-node"
UNIQUE_DIR="${BOOT_NODE}"

PRIVILEGED_HELPER_TOOLS="/Library/PrivilegedHelperTools"

ORG="org.acidanthera.nvramhook"
NVRAMDUMP="/Library/PrivilegedHelperTools/${ORG}.nvramdump.helper"

DAEMON_PLIST="/Library/LaunchDaemons/${ORG}.daemon.plist"
AGENT_PLIST="/Library/LaunchAgents/${ORG}.agent.plist"

HELPER="/Library/PrivilegedHelperTools/${ORG}.helper"
LOGDIR="/var/log/${ORG}.launchd"
LOGFILE="${LOGDIR}/launchd.log"

SELFDIR="$(/usr/bin/dirname "${0}")"
SELFNAME="$(/usr/bin/basename "${0}")"

BOOT_MOUNT="/tmp/boot-mount"

USERID=$(id -u)

for arg;
do
  case $arg in
    install )
      INSTALL=1
      ;;
    uninstall )
      UNINSTALL=1
      ;;
    agent )
      AGENT=1
      PREFIX="Agent"
      ;;
    daemon )
      DAEMON=1
      PREFIX="Daemon"
      ;;
    logout )
      LOGOUT=1
      ;;
    status )
      STATUS=1
      ;;
    * )
      usage
      exit 0
      ;;
  esac
done

# defaults
if [ ! "$AGENT"  = "1" ] &&
   [ ! "$DAEMON" = "1" ] &&
   [ ! "$LOGOUT" = "1" ] ; then
  if [ "$INSTALL"   = "1" ] ||
     [ "$UNINSTALL" = "1" ] ; then
    DAEMON=1
  else
    LOGOUT=1
  fi
fi

if [ "$SELFNAME" == "Launchd.command" ] ; then
  cd "${SELFDIR}" || abort "Failed to enter working directory!"

  if [ ! -x ./nvramdump ] ; then
    abort "nvramdump is not found!"
  fi

  USE_NVRAMDUMP="./nvramdump"
  INSTALLED=0
else
  USE_NVRAMDUMP="${NVRAMDUMP}"
  INSTALLED=1
fi

install() {
  FAIL="Failed to install!"

  if [ ! -d "${PRIVILEGED_HELPER_TOOLS}" ] ; then
    sudo mkdir "${PRIVILEGED_HELPER_TOOLS}" || abort "${FAIL}"
  fi


  if [ "$LOGOUT" = "1" ] ; then
    # logout hook from more permanent location if available
    if [ "$AGENT" = "1" ] ||
       [ "$DAEMON" = "1" ] ; then
      HOOKPATH="${HELPER}"
    else
      HOOKPATH="$(pwd)/${SELFNAME}"
    fi
    sudo defaults write com.apple.loginwindow LogoutHook "${HOOKPATH}" || abort "${FAIL}"
  fi

  sudo cp "${SELFNAME}" "${HELPER}" || abort "${FAIL}"
  sudo cp nvramdump "${NVRAMDUMP}"  || abort "${FAIL}"

  if [ "$AGENT" = "1" ] ; then
    sed "s/\$LABEL/${ORG}.agent/g;s/\$HELPER/$(sed 's/\//\\\//g' <<< $HELPER)/g;s/\$PARAM/agent/g;s/\$LOGFILE/$(sed 's/\//\\\//g' <<< $LOGFILE)/g" "Launchd.command.plist" > "/tmp/Launchd.command.plist" || abort "${FAIL}"
    sudo cp "/tmp/Launchd.command.plist" "${AGENT_PLIST}" || abort "${FAIL}"
    rm -f /tmp/Launchd.command.plist
  fi

  if [ "$DAEMON" = "1" ] ; then
    sed "s/\$LABEL/${ORG}.daemon/g;s/\$HELPER/$(sed 's/\//\\\//g' <<< $HELPER)/g;s/\$PARAM/daemon/g;s/\$LOGFILE/$(sed 's/\//\\\//g' <<< $LOGFILE)/g" "Launchd.command.plist" > "/tmp/Launchd.command.plist" || abort "${FAIL}"
    sudo cp "/tmp/Launchd.command.plist" "${DAEMON_PLIST}" || abort "${FAIL}"
    rm -f /tmp/Launchd.command.plist
  fi

  if [ ! -d "${LOGDIR}" ] ; then
    sudo mkdir "${LOGDIR}" || abort "${FAIL}"
  fi

  if [ ! -f "${LOGFILE}" ] ; then
    sudo touch "${LOGFILE}" || abort "${FAIL}"
  fi

  if [ "$AGENT" = "1" ] ; then
    # Allow agent to access log
    sudo chmod 666 "${LOGFILE}" || abort "${FAIL}"
  fi

  if [ "$AGENT" = "1" ] ; then
    # sudo for agent commands to get better logging of errors
    sudo launchctl bootstrap gui/${USERID} "${AGENT_PLIST}" || abort "${FAIL}"
  fi

  if [ "$DAEMON" = "1" ] ; then
    sudo launchctl load "${DAEMON_PLIST}" || abort "${FAIL}"
  fi

  echo "Installed."
}

if [ "$INSTALL" = "1" ] ; then
  install
  exit 0
fi

uninstall() {
  UNINSTALLED=1

  if [ "$LOGOUT" = "1" ] ; then
    sudo defaults delete com.apple.loginwindow LogoutHook || UNINSTALLED=0
  fi

  if [ "$AGENT" = "1" ] ; then
    sudo launchctl bootout gui/${USERID} "${AGENT_PLIST}" || UNINSTALLED=0
    sudo rm -f "${AGENT_PLIST}" || UNINSTALLED=0
  fi

  if [ "$DAEMON" = "1" ] ; then
    # Special value in saved device node so that nvram.plist is not upadated at uninstall
    sudo /usr/sbin/nvram "${BOOT_NODE}=null" || abort "Failed to save null boot device!"
    sudo launchctl unload "${DAEMON_PLIST}" || UNINSTALLED=0
    sudo rm -f "${DAEMON_PLIST}" || UNINSTALLED=0
  fi

  sudo rm -f "${HELPER}" || UNINSTALLED=0
  sudo rm -f "${NVRAMDUMP}" || UNINSTALLED=0

  if [ "$UNINSTALLED" = "1" ] ; then
    echo "Uninstalled."
  else
    echo "Could not uninstall!"
  fi
}

if [ "$UNINSTALL" = "1" ] ; then
  uninstall
  exit 0
fi

status() {
  if [ ! "$AGENT" = "1" ] &&
     [ ! "$DAEMON" = "1" ] ; then
    # summary info
    echo "Daemon pid = $(sudo launchctl print "system/${ORG}.daemon" 2>/dev/null | sed -n 's/.*pid = *//p')"
    echo "Agent pid = $(launchctl print "gui/${USERID}/${ORG}.agent" 2>/dev/null | sed -n 's/.*pid = *//p')"
    echo "LogoutHook = $(sudo defaults read com.apple.loginwindow LogoutHook 2>/dev/null)"
  else
    # detailed info on whatever is selected
    if [ "$AGENT" = "1" ] ; then
      launchctl print "gui/${USERID}/${ORG}.agent"
    fi

    if [ "$DAEMON" = "1" ] ; then
      sudo launchctl print "system/${ORG}.daemon"
    fi
  fi
}

if [ "$STATUS" = "1" ] ; then
  status
  exit 0
fi

# Save some diskutil info in emulated NVRAM for use at daemon shutdown:
#  - We can access diskutil normally at agent startup and at logout hook;
#  - We cannot use it on daemon shutdown because:
#     "Unable to run because unable to use the DiskManagement framework.
#     Common reasons include, but are not limited to, the DiskArbitration
#     framework being unavailable due to being booted in single-user mode."
#  - At daemon startup, diskutil works but the device may not be ready
#    immediately, but macOS restarts us quickly (~5s) and then we can run.
# Note that saving any info for use at process shutdown, if not running in daemon
# (sudo), would have to go into e.g. a file, not nvram.
saveMount() {
  UUID="$(/usr/sbin/nvram "${BOOT_PATH}" | sed 's/.*GPT,\([^,]*\),.*/\1/')"
  if [ "$(printf '%s' "${UUID}" | /usr/bin/wc -c)" -eq 36 ] && [ -z "$(echo "${UUID}" | sed 's/[-0-9A-F]//g')" ] ; then
    node="$(/usr/sbin/diskutil info "${UUID}" | sed -n 's/.*Device Node: *//p')"

    if [ "${node}" = "" ] ; then
      abort "Cannot access device node!"
    fi

    doLog "Found boot device at ${node}"

    if [ "${1}" = "1" ] ; then
      # On earlier macOS (at least Mojave in VMWare) we have an intermittent
      # problem where msdos/FAT kext is occasionally not available when we try
      # to mount the drive on daemon exit; try to fix this by mounting and
      # unmounting now.
      mount_path=$(mount | sed -n "s:${node} on \(.*\) (.*$:\1:p")
      if [ ! "${mount_path}" = "" ] ; then
        doLog "Early mount not needed, already mounted at ${mount_path}"
      else
        /usr/sbin/diskutil mount "${node}" 1>/dev/null || abort "Early mount failed!"
        /usr/sbin/diskutil unmount "${node}" 1>/dev/null || abort "Early unmount failed!"
        doLog "Early mount/unmount succeeded"
      fi

      # Use hopefully emulated NVRAM as temporary storage for the boot
      # device node discovered with diskutil.
      # If we are in emulated NVRAM, should not appear at next boot as
      # nvramdump does not write values from OC GUID back to nvram.plist.
      sudo /usr/sbin/nvram "${BOOT_NODE}=${node}" || abort "Failed to store boot device!"
    fi
  else
    abort "Illegal UUID or unknown loader!"
  fi
}

saveNvram() {
  if [ "${1}" = "1" ] ; then
    # . matches tab, note that \t cannot be used in earlier macOS (e.g Mojave)
    node=$(nvram "$BOOT_NODE" | sed -n "s/${BOOT_NODE}.//p")
    if [ "$INSTALLED" = "0" ] ; then
      # don't trash saved value if daemon is live
      launchctl print "system/${ORG}.daemon" 2>/dev/null 1>/dev/null || sudo /usr/sbin/nvram -d "$BOOT_NODE"
    else
      sudo /usr/sbin/nvram -d "$BOOT_NODE"
    fi
  fi

  if [ "${node}" = "" ] ; then
    abort "Cannot access saved device node!"
  elif [ "${node}" = "null" ] ; then
    sudo /usr/sbin/nvram "${BOOT_NODE}=" || abort "Failed to remove boot node variable!"
    doLog "Uninstalling…"
    return
  fi

  mount_path=$(mount | sed -n "s:${node} on \(.*\) (.*$:\1:p")
  if [ ! "${mount_path}" = "" ] ; then
    doLog "Already mounted at ${mount_path}"
  else
    # use reasonably assumed unique path
    mount_path="/Volumes/${UNIQUE_DIR}"
    sudo mkdir "${mount_path}" || abort "Failed to make directory!"
    sudo mount -t msdos "${node}" "${mount_path}" || abort "Failed to mount!"
    doLog "Successfully mounted at ${mount_path}"
  fi

  rm -f /tmp/nvram.plist
  ${USE_NVRAMDUMP} || abort "failed to save nvram.plist!"

  cp /tmp/nvram.plist "${mount_path}/nvram.plist" || abort "Failed to copy nvram.plist!"
  doLog "nvram.plist saved"

  rm -f /tmp/nvram.plist

  date >> "${mount_path}/${2}.hook.log" || abort "Failed to write to ${2}.hook.log!"

  # We would like to unmount here, but umount fails with "Resource busy"
  # and diskutil is not available. This should not cause any problem except
  # that the boot drive will be left mounted at the unique path if the
  # daemon process gets killed (the process would then by restarted by macOS
  # and NVRAM should still be saved at exit).
}

onComplete() {
  doLog "Trap ${1}"

  if [ "$DAEMON" = "1" ] ; then
    saveNvram 1 "daemon"
  fi

  doLog "Ended."

  # Needed if running directly (launchd kills any orphaned child processes by default).
  kill $(jobs -p)

  exit 0
}

if [ "${LOGOUT}" = "1" ] ; then
  #LOG="${SELFDIR}/error.log"
  saveMount 0
  saveNvram 0 "logout"
  exit 0
fi

# Useful for trapping all signals to see what we get.
#for s in {1..31} ;do trap "onComplete $s" $s ;done

# Trap CTRL+C for testing when running in immediate mode, and trap agent/daemon termination.
# Separate trap commands so we can log which was caught.
trap "onComplete SIGINT" SIGINT
trap "onComplete SIGTERM" SIGTERM

doLog "Starting…"

if [ "$DAEMON" = "1" ] ; then
  saveMount 1
fi

while true
do
    doLog "Running…"

    # https://apple.stackexchange.com/a/126066/113758
    # Only works from Yosemite upwards.
    sleep $RANDOM & wait
done
