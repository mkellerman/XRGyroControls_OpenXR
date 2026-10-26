import Foundation
import SimulatorKit
import CoreSimulator
import AppKit

import os

@objc(SimulatorSupport)
public class SimulatorSupport : NSObject, SimDeviceUserInterfacePlugin {
    
    private let device: SimDevice
    private let hid_client: NSObject?

    @objc init(with device: SimDevice) {
        self.device = device
        os_log("XRGyroControls: Initialized with device: \(device)")
        self.hid_client = ObjCBridge_CreateHIDClient(device)
        os_log("XRGyroControls: Initialized HID client")
        super.init()

        guard hid_client != nil else { return }

        DispatchQueue.global().async {
            self.openxr_thread()
        }
    }

    @objc func openxr_thread() {
        Thread.sleep(forTimeInterval: 1.0)

        //print("OpenXR thread start!")

        ObjCBridge_Startup();

        var asdf: Int = 0
        var last_system_button: Int32 = 0;
        while (true) {
            let pose = ObjCBridge_Loop().pointee;
            let norm = pose.l_qx * pose.l_qx + pose.l_qy * pose.l_qy + pose.l_qz * pose.l_qz + pose.l_qw * pose.l_qw
            if !norm.isFinite || norm < 0.5 {
                Thread.sleep(forTimeInterval: 0.01)
                continue
            }
            //let pose = self.openxr_wrapper.get_data()

            if (pose.system_button != 0 && pose.system_button != last_system_button) {
                ObjCBridge_HomeButtonPress()


            }
            last_system_button = pose.system_button
            
            send(IndigoHIDMessage.pose(pose.l_x, pose.l_y, pose.l_z, pose.l_qx, pose.l_qy, pose.l_qz, pose.l_qw))
            send(IndigoHIDMessage.manipulator(asdf, pose))
            if (asdf < 60) {
                //ObjCBridge_HomeButtonPress();
            }
            else {
            }

            asdf += 1
            if (asdf > 120) {
                asdf = 0
                //ObjCBridge_HomeButtonPressUp();
            }

            Thread.sleep(forTimeInterval: 0.004)
            //Thread.sleep(forTimeInterval: 0.5)
        }

        ObjCBridge_Shutdown();
        //print("OpenXR thread end!")
        //self.openxr_wrapper.cleanup()
    }
    
    func send_test_message(_ cnt: Int) {
        send(IndigoHIDMessage.pose(0.0, Float(cnt) / 1000, 0.0, 0.0, 0.0, 0.0, 1.0))
    }

    private func send(_ message: IndigoHIDMessage) {
        guard let client = hid_client else { return }
        let pointer = message.as_struct()
        defer { pointer.deallocate() }
        _ = ObjCBridge_SendHID(client, pointer, MemoryLayout<IndigoHIDMessageStruct>.size)
    }
    
    @objc func overlayView() -> NSView {
        return NSView()
    }
    
    @objc func toolbar() -> NSToolbar {
        return NSToolbar()
    }

    /*
    @objc func gameControllerValueChanged(element: NSObject) -> Int {
        return 1;
    }

    @objc func gameControllerConnectionStateChanged(element: NSObject) -> Int {
        return 1;
    }
    */
}
