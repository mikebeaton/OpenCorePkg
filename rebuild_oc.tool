#!/bin/bash

read OC_VER <<< $(grep OPEN_CORE_VERSION\ *\"[0-9]\.[0-9]\.[0-9]\" Include/Acidanthera/Library/OcMainLib.h | cut -d \" -f 2)

if [ "$OC_VER" == "" ]; then
  echo "Cannot extract OC version"
  exit -1
fi

echo "OpenCore $OC_VER"

TARGET="DEBUG"
OC_VOLUME_DIR=/Volumes/OPENCORE

SKIP_BUILD=0

while true; do
  if [ "$1" == "--skip-build" ] || [ "$1" == "-s" ]; then
    SKIP_BUILD=1
    shift
  elif [ "$1" == "--ovmf" ] || [ "$1" == "-o" ]; then
    OC_VOLUME_DIR=~/OPENCORE
    TARGET="NOOPT"
    shift
  elif [ "$1" == "--dir" ] || [ "$1" == "-d" ]; then
    shift
    if [ "$1" != "" ]; then
      OC_VOLUME_DIR=$1
      shift
    else
      echo "No output dir specified" && exit 1
    fi
  else
    break
  fi
done

if ! [ -d $OC_VOLUME_DIR ]; then
  echo "Target dir ${OC_VOLUME_DIR} does not exist" && exit 1
fi

if [ "$1" != "" ]; then
  TARGET=$1
fi

eval "$(git status | grep "On branch" | awk -F '[ ]' '{print "MY_BRANCH=" $3}')"

if [ "$MY_BRANCH" = "" ]; then
  eval "$(git status | grep "HEAD detached at" | awk -F '[ ]' '{print "MY_BRANCH=" $4}')"
  if [ "$MY_BRANCH" = "" ]; then
    echo "Not on any git branch or tag!"
    exit 1
  fi
fi

BUILD_DIR="./UDK/Build/OpenCorePkg/${TARGET}_XCODE5/X64"

BASE=OpenCore-${OC_VER}-${TARGET}
VER_ZIP_FILE=~/OC/${BASE}-${MY_BRANCH}.zip
ZIP_FILE=~/OC/${BASE}.zip
UNZIP_DIR=~/OC/${BASE}

if [ ! -f $VER_ZIP_FILE ]; then
  echo "Cannot find ${VER_ZIP_FILE}, using 'rebuild_all -s' ..."
  echo
  # does not matter if -s is repeated
  ./rebuild_all.tool -s $@ || exit 1
  echo
elif diff -q ${VER_ZIP_FILE} ${ZIP_FILE} &>/dev/null; then
  >&2 echo "Zip is correct for ${MY_BRANCH} ${TARGET}."
else
  >&2 echo "Zip is not correct for ${MY_BRANCH} ${TARGET}."
  >&2 echo "Removing non-matching unzipped dir..."
  rm -rf $UNZIP_DIR || exit 1

  echo "Copying ${BASE}-${MY_BRANCH}.zip to ${BASE}.zip..."
  cp ${VER_ZIP_FILE} ${ZIP_FILE} || exit 1

  echo "Unzipping..."
  unzip ${ZIP_FILE} -d ${UNZIP_DIR} 1>/dev/null || exit 1

  echo "Copying ${MY_BRANCH} ${TARGET} files to ${OC_VOLUME_DIR}..."
  cp -r ${UNZIP_DIR}/X64/EFI ${OC_VOLUME_DIR} || exit 1
fi

# remove files we will rebuild
echo "Removing OpenCore.efi, OpenCanopy.efi, OpenShell.efi, AudioDxe.efi, OpenLinuxBoot.efi, ResetNvramEntry.efi, ToggleSipEntry.efi, OpenVariableRuntime.efi from ${OC_VOLUME_DIR}..."
#rm ${OC_VOLUME_DIR}/EFI/BOOT/BOOTx64.efi
rm ${OC_VOLUME_DIR}/EFI/OC/OpenCore.efi
rm ${OC_VOLUME_DIR}/EFI/OC/Drivers/OpenCanopy.efi
rm ${OC_VOLUME_DIR}/EFI/OC/Tools/OpenShell.efi
rm ${OC_VOLUME_DIR}/EFI/OC/Drivers/AudioDxe.efi
rm ${OC_VOLUME_DIR}/EFI/OC/Drivers/OpenLinuxBoot.efi
rm ${OC_VOLUME_DIR}/EFI/OC/Drivers/ResetNvramEntry.efi
rm ${OC_VOLUME_DIR}/EFI/OC/Drivers/ToggleSipEntry.efi
rm ${OC_VOLUME_DIR}/EFI/OC/Drivers/OpenVariableRuntime.efi

if [ "$SKIP_BUILD" != "1" ]; then
  # rebuild them
  echo "Rebuilding..."
  cd ./UDK
  source edksetup.sh BaseTools || exit 1
  build -a X64 -b ${TARGET} -t XCODE5 -p OpenCorePkg/OpenCorePkg.dsc || exit 1
  cd ..
fi

# put them back
echo "Copying ${MY_BRANCH} ${TARGET} OpenCore.efi, OpenCanopy.efi, OpenShell.efi, AudioDxe.efi, OpenLinuxBoot.efi, ResetNvramEntry.efi, ToggleSipEntry.efi, OpenVariableRuntime.efi to ${OC_VOLUME_DIR}..."
#cp ${BUILD_DIR}/Bootstrap.efi ${OC_VOLUME_DIR}/EFI/BOOT/BOOTx64.efi || exit 1
cp ${BUILD_DIR}/OpenCore.efi ${OC_VOLUME_DIR}/EFI/OC || exit 1
cp ${BUILD_DIR}/OpenCanopy.efi ${OC_VOLUME_DIR}/EFI/OC/Drivers || exit 1
cp ${BUILD_DIR}/Shell.efi ${OC_VOLUME_DIR}/EFI/OC/Tools/OpenShell.efi || exit 1
cp ${BUILD_DIR}/AudioDxe.efi ${OC_VOLUME_DIR}/EFI/OC/Drivers || exit 1
cp ${BUILD_DIR}/OpenLinuxBoot.efi ${OC_VOLUME_DIR}/EFI/OC/Drivers || exit 1
cp ${BUILD_DIR}/ResetNvramEntry.efi ${OC_VOLUME_DIR}/EFI/OC/Drivers || exit 1
cp ${BUILD_DIR}/ToggleSipEntry.efi ${OC_VOLUME_DIR}/EFI/OC/Drivers || exit 1
cp ${BUILD_DIR}/OpenVariableRuntime.efi ${OC_VOLUME_DIR}/EFI/OC/Drivers || exit 1

# Mark binaries to be recognisable by OcBootManagementLib.
bootsig="./Library/OcBootManagementLib/BootSignature.bin"
efiOCBMs=(
#  "/EFI/BOOT/BOOTx64.efi"
  "/EFI/OC/OpenCore.efi"
  )
for efiOCBM in "${efiOCBMs[@]}"; do
  echo "Signing ${efiOCBM}..."
  dd if="${bootsig}" \
     of="${OC_VOLUME_DIR}${efiOCBM}" seek=64 bs=1 count=56 conv=notrunc || exit 1
done

echo "Done."
