# XRGyroControls_OpenXR

## Binary Release Instructions

- Disable SIP
- Disable library validation (`sudo defaults write /Library/Preferences/com.apple.security.libraryvalidation.plist DisableLibraryValidation -bool true`)
 - Install brew dependencies: `brew install gsed`
- You must have Xcode 15.1 Beta 2 installed to `/Applications`, or `XCODE_BETA_PATH` set to your Xcode 15.1 Beta 2 app bundle (eg, `export XCODE_BETA_PATH=/Applications/Xcode-beta.app`)
- Extract [binary release zip](https://github.com/shinyquagsire23/XRGyroControls_OpenXR/releases) any folder
- Using terminal, navigate to the extracted folder.
- Run `./install.sh`. This will sign all files for your machine, and adjust loading paths for the current folder. This only needs to be done once, but doing it multiple times will not cause any issues.
- Run `./run.sh`. This will start the simulator, and the UI should now be absent.
- Run `./inject.sh`. This will inject into the compositor.

By default, Monado's simulated headset will run in the simulator. This headset gyrates its position slightly. To use Quest Link, plug in your headset, and run `./inject.sh`. The simulator will show the Apple logo until the headset enters Quest Link.

## Known Issues

- visionOS 1.0 Beta 2 semitransparency is extremely flickery. Set the "Reduce transparency" setting in Settings > Accessibility > Display & Text Size.
- The visionOS simulator window **must** be focused in order to use the Home button on controllers.
- The gaze ray gets stuck on the left controller when it disconnects. Press Menu to reset it.
- The Quest Link video stream sometimes becomes juddery after sleeping the headset.
- Sometimes the visionOS simulator will fail to run after too many launches, due to a MetalSim bug? Requires an OS restart.

## Accessing the old vision sim UI again
- Run `./uninstall.sh`

## Building Instructions

Before running anything in this repository, you will want to follow the instructions at https://gist.github.com/shinyquagsire23/3c68aecd872cc7ac21c28e950245dbd2 for setting up a *normal* Quest Link on macOS environment. This includes stuff like setting up MoltenVK.

Before building, do the following:
 - Disable SIP
 - Disable library validation (`sudo defaults write /Library/Preferences/com.apple.security.libraryvalidation.plist DisableLibraryValidation -bool true`)
 - Set `MONADO_BUILD_DIR` to your `monado/build` directory, ie `/Users/maxamillion/workspace/monado/build`
 - Ensure monado in `MONADO_BUILD_DIR` is compiled with `cmake .. -DXRT_ENABLE_GPL=1 -DXRT_BUILD_DRIVER_EUROC=0 -DXRT_BUILD_DRIVER_NS=0 -DXRT_BUILD_DRIVER_PSVR=0 -DXRT_HAVE_OPENCV=0 -DXRT_HAVE_XCB=0 -DXRT_HAVE_XLIB=0 -DXRT_HAVE_XRANDR=0 -DXRT_HAVE_SDL2=0  -DXRT_HAVE_VT=0 -DXRT_FEATURE_WINDOW_PEEK=0 -DXRT_BUILD_DRIVER_QWERTY=0 -DXRT_BUILD_DRIVER_WMR=0 -DXRT_FEATURE_SERVICE=0 -DXRT_FEATURE_STEAMVR_PLUGIN=0 -DXRT_MODULE_IPC=0`
 - Set `VULKAN_SDK`
 - If your Xcode-beta.app is not located at `/Applications/Xcode-beta.app`, set the `XCODE_BETA_PATH` env var to your Xcode-beta.app path.
 - Install brew dependencies: `brew install autoconf automake libtool gsed`

Once all the prerequisites are done, run `./build.sh`.

## Build Troubleshooting

- This repo has only been tested on macOS Sonoma 14.0, Xcode 15.1 beta 2, and M1 macOS machines.
  - x86_64 is almost certainly broken currently -- It will need to *not* use the libusb patches and *not* use `IOUSBLib_ios_hax.dylib`. In theory it should be able to load IOUSBLib from the dylib cache w/o issues. It might also Just Work without any specific changes, dunno.
- If `libusb` fails to build, ensure you have all dependencies (especially `autoconf` and `automake`), delete the `libusb` directory, and then run `./build.sh` again
- If `libSim2OpenXR.dylib` fails to build, ensure you have all dependencies (especially `autoconf` and `automake`), delete the `libusb` directory, and then run `./build.sh` again.

## Running

Run `build_run.sh` to reset and then inject into the visionOS simulator. Run `build_inject.sh` to just inject into an existing instance.