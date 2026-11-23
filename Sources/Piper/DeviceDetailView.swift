// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

enum DetailTab: String, CaseIterable, Identifiable {
    case resolutions = "Resolutions"
    case buttons = "Buttons"
    case leds = "LEDs"
    case advanced = "Advanced"

    var id: String { rawValue }
}

struct DeviceDetailView: View {
    @ObservedObject var device: Device
    @ObservedObject var profile: Profile
    @AppStorage("detailTab") private var tab: DetailTab = .resolutions

    private var tabs: [DetailTab] {
        DetailTab.allCases.filter { $0 != .leds || !profile.leds.isEmpty }
    }

    var body: some View {
        VStack(spacing: 0) {
            if !device.isConnected {
                Banner(symbol: "bolt.horizontal.circle", text: "\(device.name) was disconnected. Reconnect it to make changes.")
            }
            ProfileHeader(device: device, profile: profile)
            Divider()
            Group {
                switch tabs.contains(tab) ? tab : .resolutions {
                case .resolutions: ResolutionsView(device: device, profile: profile)
                case .buttons: ButtonsView(device: device, profile: profile)
                case .leds: LedsView(device: device, profile: profile)
                case .advanced: AdvancedView(device: device, profile: profile)
                }
            }
            .disabled(!device.isEditable)
        }
        .navigationTitle(device.name)
        .navigationSubtitle(profile.displayName)
        .focusedSceneObject(device)
        .toolbar {
            ToolbarItem(placement: .principal) {
                Picker("Section", selection: $tab) {
                    ForEach(tabs) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .fixedSize()
            }
            ToolbarItem(placement: .primaryAction) {
                if device.isCommitting {
                    ProgressView().controlSize(.small).padding(.horizontal, 8)
                        .help("Writing to the device…")
                } else {
                    Button("Apply") { device.commit() }
                        .buttonStyle(.borderedProminent)
                        .disabled(!device.isDirty || !device.isEditable)
                        .help("Write the changes to the mouse (⌘S)")
                }
            }
        }
        .alert(
            "Something went wrong",
            isPresented: Binding(get: { device.lastError != nil }, set: { if !$0 { device.lastError = nil } }),
            actions: { Button("OK") { device.lastError = nil } },
            message: { Text(device.lastError ?? "") }
        )
    }
}

private struct ProfileHeader: View {
    @ObservedObject var device: Device
    @ObservedObject var profile: Profile

    var body: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(profile.displayName).font(.title2.weight(.semibold))
                Text(statusText).font(.callout).foregroundStyle(.secondary)
            }
            Spacer()
            if profile.canDisable {
                Toggle("Enabled", isOn: Binding(get: { profile.isEnabled }, set: { profile.setEnabled($0) }))
                    .toggleStyle(.switch)
                    .disabled(profile.isActive || !device.isEditable)
                    .help(profile.isActive ? "The active profile can't be disabled" : "Whether the mouse's profile button can switch to this profile")
            }
            if !profile.isActive {
                Button("Make Active") { profile.makeActive() }
                    .disabled(!profile.isEnabled || !device.isEditable)
                    .help("Switch the mouse to this profile")
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 14)
    }

    private var statusText: String {
        var parts: [String] = []
        parts.append(profile.isActive ? "Active profile" : (profile.isEnabled ? "Inactive" : "Disabled"))
        if profile.isDirty { parts.append("unsaved changes") }
        return parts.joined(separator: " · ")
    }
}

struct Banner: View {
    let symbol: String
    let text: String

    var body: some View {
        HStack {
            Image(systemName: symbol)
            Text(text)
            Spacer()
        }
        .font(.callout)
        .padding(.horizontal, 20)
        .padding(.vertical, 8)
        .background(.yellow.opacity(0.25))
    }
}
