#!/bin/sh

#
# Copyright © 2020 Rodion Shingarev. All rights reserved.
# Slight optimizations by PMheart and vit9696.
#

if [ "$1" = "install" ]; then
  SELFNAME=$(basename "$0")
  SELFDIR=$(dirname "$0")
  cd "$SELFDIR" || exit 1
  sudo defaults write com.apple.loginwindow LogoutHook "$(pwd)/${SELFNAME}"
  exit 0
fi

if [ ! -x /usr/bin/dirname ] || [ ! -x /usr/sbin/nvram ] || [ ! -x /bin/rm ] || [ ! -x /usr/sbin/diskutil ] || [ ! -x /bin/cp ]  ; then
  abort "Unix environment is broken!"
fi

thisDir="$(/usr/bin/dirname "${0}")"
cd "${thisDir}" || abort "Failed to enter working directory!"

if [ ! -x ./nvramdump ]; then
  abort "nvramdump is not found!"
fi

ERROR_LOG="${thisDir}/error.log"

dolog() {
  echo "$(date) ${1}" >> "${ERROR_LOG}"
}

abort() {
  dolog "Fatal error: ${1}"
  exit 1
}

dolog "Saving..."

rm -f /tmp/nvram.plist
./nvramdump || abort "failed to save nvram.plist!"

UUID="$(nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path | /usr/bin/sed 's/.*GPT,\([^,]*\),.*/\1/')"
if [ "$(printf '%s' "${UUID}" | /usr/bin/wc -c)" -eq 36 ] && [ -z "$(echo "${UUID}" | /usr/bin/sed 's/[-0-9A-F]//g')" ]; then
  /usr/sbin/diskutil mount "${UUID}" 2>>"${ERROR_LOG}" >> "${ERROR_LOG}" || abort "Failed to mount ${UUID}!"
  p="$(/usr/sbin/diskutil info "${UUID}" | /usr/bin/sed -n 's/.*Mount Point: *//p')"
  echo ${p} > /tmp/boot-mount
  #if ! cmp -s /tmp/nvram.plist "${p}/nvram.plist"
  #then
    ###/bin/cp /tmp/nvram.plist "${p}/nvram.plist" || abort "Failed to copy nvram.plist!"
    /bin/cp /tmp/nvram.plist "${p}/nvram.logout.plist" || abort "Failed to copy nvram.plist!"
  #fi
  date >> "${p}/logout.hook.log"
  #/usr/sbin/diskutil unmount "${UUID}" || abort "Failed to unmount ${UUID}!"

  dolog "Done."
  exit 0
else
  abort "Illegal UUID or unknown loader!"
fi
