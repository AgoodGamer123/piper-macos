// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        if let context = model.context {
            MainView(context: context)
        } else {
            EmptyStateView(
                symbol: "exclamationmark.triangle",
                title: "libratbag could not start",
                message: "Piper's device library failed to initialize. Start Piper from Terminal with PIPER_RATBAG_DEBUG=1 to see why."
            )
        }
    }
}

private struct MainView: View {
    @EnvironmentObject private var model: AppModel
    @ObservedObject var context: RatbagContext

    var body: some View {
        NavigationSplitView {
            List(selection: $model.selection) {
                ForEach(context.devices) { device in
                    Section {
                        ForEach(device.profiles) { profile in
                            ProfileRow(profile: profile)
                                .tag(ProfileSelection(deviceID: device.id, profileIndex: profile.index))
                        }
                    } header: {
                        DeviceHeader(device: device)
                    }
                }
            }
            .listStyle(.sidebar)
            .navigationSplitViewColumnWidth(min: 200, ideal: 230)
            .overlay {
                if context.devices.isEmpty {
                    Text(context.hasScanned ? "No devices" : "Looking for devices…")
                        .foregroundStyle(.secondary)
                }
            }
        } detail: {
            if let selection = model.selection,
               let device = model.device(for: selection),
               selection.profileIndex < device.profiles.count
            {
                DeviceDetailView(device: device, profile: device.profiles[selection.profileIndex])
                    .id(selection)
            } else if context.devices.isEmpty {
                EmptyStateView(
                    symbol: "computer.mouse",
                    title: context.hasScanned ? "No supported mouse found" : "Looking for devices…",
                    message: "Connect your mouse, or its wireless receiver. A wireless mouse has to be switched on and awake; Piper notices it within a few seconds."
                )
            } else {
                EmptyStateView(symbol: "sidebar.left", title: "Select a profile", message: "")
            }
        }
        .onAppear { context.start() }
        .onReceive(context.$devices) { devices in
            selectDefaultIfNeeded(devices)
        }
    }

    private func selectDefaultIfNeeded(_ devices: [Device]) {
        if let selection = model.selection, devices.contains(where: { $0.id == selection.deviceID }) {
            return
        }
        if let device = devices.first, let profile = device.activeProfile {
            model.selection = ProfileSelection(deviceID: device.id, profileIndex: profile.index)
        } else {
            model.selection = nil
        }
    }
}

private struct DeviceHeader: View {
    @ObservedObject var device: Device

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "computer.mouse")
            Text(device.name)
            if device.isDirty {
                Circle().fill(.orange).frame(width: 6, height: 6)
                    .help("Changes not yet applied")
            }
        }
    }
}

private struct ProfileRow: View {
    @ObservedObject var profile: Profile

    var body: some View {
        HStack {
            Label(profile.displayName, systemImage: profile.isActive ? "checkmark.circle.fill" : "circle")
                .foregroundStyle(profile.isEnabled ? Color.primary : Color.secondary)
            Spacer()
            if profile.isDirty {
                Circle().fill(.orange).frame(width: 6, height: 6)
            }
        }
        .help(profile.isActive ? "The profile the mouse is using" : (profile.isEnabled ? "" : "Disabled profile"))
    }
}

struct EmptyStateView: View {
    let symbol: String
    let title: String
    let message: String

    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: symbol)
                .font(.system(size: 44, weight: .light))
                .foregroundStyle(.secondary)
            Text(title).font(.title2)
            if !message.isEmpty {
                Text(message)
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: 380)
            }
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
