#!/bin/zsh
PWD=$(pwd)

# Be sure to follow https://gist.github.com/shinyquagsire23/3c68aecd872cc7ac21c28e950245dbd2

# Monado build config
#cmake .. -DXRT_ENABLE_GPL=1 -DXRT_BUILD_DRIVER_EUROC=0 -DXRT_BUILD_DRIVER_NS=0 -DXRT_BUILD_DRIVER_PSVR=0 -DXRT_HAVE_OPENCV=0 -DXRT_HAVE_XCB=0 -DXRT_HAVE_XLIB=0 -DXRT_HAVE_XRANDR=0 -DXRT_HAVE_SDL2=0  -DXRT_HAVE_VT=0 -DXRT_FEATURE_WINDOW_PEEK=0 -DXRT_BUILD_DRIVER_QWERTY=0 -DXRT_BUILD_DRIVER_WMR=0 -DXRT_FEATURE_SERVICE=0 -DXRT_FEATURE_STEAMVR_PLUGIN=0 -DXRT_MODULE_IPC=0

# These env vars can interfere w/ building
unset MACOSX_DEPLOYMENT_TARGET
#export MACOSX_DEPLOYMENT_TARGET="13.0"

if [[ -z "${XCODE_BETA_PATH}" ]]; then
    export XCODE_BETA_PATH="/Applications/Xcode-beta.app"
fi

pushd $MONADO_BUILD_DIR
make
popd

if ! [[ -d "libusb" ]]; then
    mkdir -p libusb
    pushd libusb
    git init
    git remote add origin https://github.com/libusb/libusb.git
    git fetch --depth 1 origin 8450cc93f6c8747a36a9ee246708bf650bb762a8
    git checkout FETCH_HEAD
    git apply --whitespace=fix ../libusb.patch
    ./bootstrap.sh
    ./configure
    popd
fi

pushd libusb
make
retVal=$?
if [ $retVal -ne 0 ]; then
    exit $retVal
fi
popd

# Build simui, and if it errors then abort
export MACOSX_DEPLOYMENT_TARGET="13.0"
cmake -B build_simui -D CMAKE_BUILD_TYPE=RelWithDebInfo -D BUILD_TESTING=YES -G Ninja -S simui
ninja -C build_simui -v
retVal=$?
if [ $retVal -ne 0 ]; then
    exit $retVal
fi
unset MACOSX_DEPLOYMENT_TARGET

# Build, and if it errors then abort
cmake -B build -D CMAKE_BUILD_TYPE=RelWithDebInfo -D BUILD_TESTING=YES -G Ninja -S .
ninja -C build -v
retVal=$?
if [ $retVal -ne 0 ]; then
    exit $retVal
fi

cp build/libSim2OpenXR.dylib libSim2OpenXR.dylib

# Remove old sim stuff
rm -rf ${XCODE_BETA_PATH}/Contents/Developer/Platforms/XROS.platform/Library/Developer/CoreSimulator/Profiles/UserInterface/XRGyroControls.simdeviceui

INSTALL_NAME_TOOL=${XCODE_BETA_PATH}/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/install_name_tool
VTOOL=${XCODE_BETA_PATH}/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/vtool

# otool -L is your friend, make sure there's nothing weird
function fixup_dependency ()
{
    which_dylib=$1
    vtool_src=$2
    vtool_dst=$(basename $2)
    chmod 777 $vtool_dst
    chmod 777 $which_dylib
    cp $vtool_src $vtool_dst

    # Every framework has to be changed to remove "Versions/A/".
    # You can also swap out frameworks for interposing dylibs here.
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation /System/Library/Frameworks/CoreFoundation.framework/CoreFoundation $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/Security.framework/Versions/A/Security /System/Library/Frameworks/Security.framework/Security $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/IOKit.framework/Versions/A/IOKit /System/Library/Frameworks/IOKit.framework/IOKit $vtool_dst
    #$INSTALL_NAME_TOOL -change /System/Library/Frameworks/IOKit.framework/Versions/A/IOKit $(pwd)/IOKit_arm64.dylib $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/Metal.framework/Versions/A/Metal /System/Library/Frameworks/Metal.framework/Metal $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface /System/Library/Frameworks/IOSurface.framework/IOSurface $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/AppKit.framework/Versions/C/AppKit /System/Library/Frameworks/AppKit.framework/AppKit $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore /System/Library/Frameworks/QuartzCore.framework/QuartzCore $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics /System/Library/Frameworks/CoreGraphics.framework/CoreGraphics $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation /System/Library/Frameworks/Foundation.framework/Foundation $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/PrivateFrameworks/SoftLinking.framework/Versions/A/SoftLinking /System/Library/PrivateFrameworks/SoftLinking.framework/SoftLinking $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/VideoToolbox.framework/Versions/A/VideoToolbox /System/Library/Frameworks/VideoToolbox.framework/VideoToolbox $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices /System/Library/Frameworks/CoreServices.framework/CoreServices $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia /System/Library/Frameworks/CoreMedia.framework/CoreMedia $vtool_dst
    $INSTALL_NAME_TOOL -change /System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo /System/Library/Frameworks/CoreVideo.framework/CoreVideo $vtool_dst

    $VTOOL -remove-build-version macos -output $vtool_dst $vtool_dst
    $VTOOL -set-build-version 12 1.0 1.0 -tool ld 902.11 -output $vtool_dst $vtool_dst
    $INSTALL_NAME_TOOL -id @rpath/$vtool_dst $vtool_dst
    codesign -s - $vtool_dst --force --deep --verbose

    $INSTALL_NAME_TOOL -change @rpath/$vtool_dst @loader_path/$vtool_dst $which_dylib
    $INSTALL_NAME_TOOL -change $vtool_src @loader_path/$vtool_dst $which_dylib
}

function check_exists ()
{
    which=$1
    if ! [[ -f "$which" ]]; then
        echo "missing file `$which`"
        exit -1
    fi
}

check_exists /opt/homebrew/lib/libopenxr_loader.dylib
check_exists $MONADO_BUILD_DIR/src/xrt/targets/openxr/libopenxr_monado.dylib
check_exists $VULKAN_SDK/lib/libvulkan.1.dylib
check_exists $VULKAN_SDK/../MoltenVK/dylib/iOS/libMoltenVK.dylib
check_exists /opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib
check_exists libusb/libusb/.libs/libusb-1.0.0.dylib
check_exists /opt/homebrew/opt/x264/lib/libx264.164.dylib
check_exists /opt/homebrew/opt/cjson/lib/libcjson.1.dylib
check_exists /opt/homebrew/opt/jpeg-turbo/lib/libjpeg.8.dylib
check_exists IOUSBLib_ios_hax.dylib

# *slow chanting* hacks, hacks, HACKS **HACKS**
fixup_dependency libSim2OpenXR.dylib libSim2OpenXR.dylib
fixup_dependency libSim2OpenXR.dylib /opt/homebrew/lib/libopenxr_loader.dylib
fixup_dependency libSim2OpenXR.dylib $MONADO_BUILD_DIR/src/xrt/targets/openxr/libopenxr_monado.dylib
fixup_dependency libSim2OpenXR.dylib $VULKAN_SDK/lib/libvulkan.1.dylib
fixup_dependency libSim2OpenXR.dylib $VULKAN_SDK/../MoltenVK/dylib/iOS/libMoltenVK.dylib
fixup_dependency libSim2OpenXR.dylib /opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib

#
# libopenxr_monado.dylib fixups
#
#fixup_dependency libopenxr_monado.dylib /opt/homebrew/opt/hidapi/lib/libhidapi.0.dylib
fixup_dependency libopenxr_monado.dylib /opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib
fixup_dependency libopenxr_monado.dylib libusb/libusb/.libs/libusb-1.0.0.dylib # This must come *after* the homebrew libusb so that it copies over the unpatched one!
fixup_dependency libopenxr_monado.dylib /opt/homebrew/opt/x264/lib/libx264.164.dylib
fixup_dependency libopenxr_monado.dylib $VULKAN_SDK/lib/libvulkan.1.dylib
fixup_dependency libopenxr_monado.dylib $VULKAN_SDK/../MoltenVK/dylib/iOS/libMoltenVK.dylib
fixup_dependency libopenxr_monado.dylib /opt/homebrew/opt/cjson/lib/libcjson.1.dylib
fixup_dependency libopenxr_monado.dylib /opt/homebrew/opt/jpeg-turbo/lib/libjpeg.8.dylib

# Pulled from iPhone X recovery ramdisk, some patches were done in a hex editor.
$VTOOL -remove-build-version macos -output  IOUSBLib_ios_hax.dylib IOUSBLib_ios_hax.dylib
$VTOOL -remove-build-version ios -output  IOUSBLib_ios_hax.dylib IOUSBLib_ios_hax.dylib
$VTOOL -set-build-version 12 1.0 1.0 -tool ld 902.11 -output IOUSBLib_ios_hax.dylib IOUSBLib_ios_hax.dylib

# Fixup libusb bc we don't actually use the homebrew one
$VTOOL -remove-build-version macos -output libusb-1.0.0.dylib libusb-1.0.0.dylib
$VTOOL -set-build-version 12 1.0 1.0 -tool ld 902.11 -output libusb-1.0.0.dylib libusb-1.0.0.dylib

cp libMoltenVK_iossim.dylib libMoltenVK.dylib
fixup_dependency libMoltenVK.dylib libMoltenVK.dylib

# Fixup monado JSON
#gsed "s|REPLACE_ME|$PWD|g" openxr_monado-dev.json.template > openxr_monado-dev.json


#
# SimUI stuff
#

# Couldn't figure out how to do this with CMake, but everything gets dynamically linked so it doesn't particularly matter and we can fixup the rpaths
mkdir -p XRGyroControls.simdeviceui/Contents/MacOS/
cp build_simui/libXRGyroControls.dylib XRGyroControls.simdeviceui/Contents/MacOS/XRGyroControls
$INSTALL_NAME_TOOL -change @rpath/libSimulatorKit.dylib  @rpath/SimulatorKit.framework/Versions/A/SimulatorKit XRGyroControls.simdeviceui/Contents/MacOS/XRGyroControls

./install.sh