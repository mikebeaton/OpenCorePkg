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

if [ "$SKIP_BUILD" != "1" ]; then
  if [ -f "${BUILD_DIR}/${BASE}.zip" ]; then
    echo "Removing Build file ${BASE}.zip..."
    rm ${BUILD_DIR}/${BASE}.zip
  else
    echo "Build file ${BASE}.zip does not exist!"
  fi

  echo "Rebuilding..."
  ARCHS=(X64)
  TARGETS=(${TARGET})
  pushd .
  source ./build_oc.tool --skip-tests || exit 1
  popd 1>/dev/null
fi

if [ ! -f ${BUILD_DIR}/${BASE}.zip ] ; then
  echo "ERROR: Built ${BUILD_DIR}/${BASE}.zip does not exist."
  exit 1
fi

echo "Removing old local ${BASE}.zip..."
rm ${ZIP_FILE}

echo "Removing old local unzipped dir..."
rm -rf ${UNZIP_DIR}

echo "Copying ${BASE}.zip from build dir..."
cp ${BUILD_DIR}/${BASE}.zip ${ZIP_FILE} || exit 1

echo "Copying ${TARGET} zip to ${BASE}-${MY_BRANCH}.zip..."
cp ${ZIP_FILE} ${VER_ZIP_FILE} || exit 1

echo "Unzipping ${BASE}.zip..."
unzip ${ZIP_FILE} -d ${UNZIP_DIR} 1>/dev/null || exit 1

echo "Copying ${MY_BRANCH} ${TARGET} files to ${OC_VOLUME_DIR}..."
cp -r ${UNZIP_DIR}/X64/EFI ${OC_VOLUME_DIR} || exit 1

echo "Done."
