import Foundation
import CoreSimulator

guard CommandLine.arguments.count == 3 else {
    fputs("Usage: xr-input DEVELOPER_DIR SIMULATOR_UDID\n", stderr)
    exit(2)
}
guard let device = ObjCBridge_DeviceForUDID(CommandLine.arguments[1], CommandLine.arguments[2]) as? SimDevice else {
    exit(1)
}
// Keep the input session alive independently of Device Hub's plugin loader.
let session = SimulatorSupport(with: device)
withExtendedLifetime(session) {
    RunLoop.main.run()
}
