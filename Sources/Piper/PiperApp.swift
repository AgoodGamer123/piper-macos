// SPDX-License-Identifier: GPL-2.0-or-later

import AppKit
import SwiftUI

@main
struct PiperApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup("Piper") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 860, minHeight: 560)
        }
        .windowToolbarStyle(.unified)
        .commands {
            DeviceCommands()
            CommandGroup(replacing: .newItem) {}
            CommandGroup(replacing: .help) {
                Button("Piper for macOS on GitHub") { NSWorkspace.shared.open(AppInfo.repositoryURL) }
                Button("Supported Devices") { NSWorkspace.shared.open(AppInfo.supportedDevicesURL) }
            }
        }
    }
}

enum AppInfo {
    static let repositoryURL = URL(string: "https://github.com/AgoodGamer123/piper-macos")!
    static let supportedDevicesURL = URL(string: "https://github.com/libratbag/libratbag/tree/master/data/devices")!
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

/// What the sidebar has selected: one profile of one device
struct ProfileSelection: Hashable {
    let deviceID: String
    let profileIndex: Int
}

final class AppModel: ObservableObject {
    let context: RatbagContext?
    @Published var selection: ProfileSelection?

    init() {
        context = RatbagContext()
    }

    func device(for selection: ProfileSelection?) -> Device? {
        guard let selection else { return nil }
        return context?.devices.first { $0.id == selection.deviceID }
    }
}

/// File > Apply Changes (⌘S) for the device in the front window
struct DeviceCommands: Commands {
    @FocusedObject private var device: Device?

    var body: some Commands {
        CommandGroup(replacing: .saveItem) {
            Button("Apply Changes") { device?.commit() }
                .keyboardShortcut("s")
                .disabled(!(device?.isDirty ?? false) || !(device?.isEditable ?? false))
        }
    }
}
