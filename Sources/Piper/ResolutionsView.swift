// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// The device picture on the left, settings on the right
struct DevicePane<Content: View>: View {
    let model: String
    var highlighted: [String] = []
    @ViewBuilder var content: Content

    var body: some View {
        HStack(spacing: 0) {
            DeviceImageView(model: model, highlighted: highlighted)
                .frame(minWidth: 220, idealWidth: 300, maxWidth: 360)
                .padding(24)
            Divider()
            content
                .frame(minWidth: 380, maxWidth: .infinity)
        }
    }
}

struct ResolutionsView: View {
    @ObservedObject var device: Device
    @ObservedObject var profile: Profile

    var body: some View {
        DevicePane(model: device.model) {
            Form {
                Section {
                    ForEach(profile.resolutions) { resolution in
                        ResolutionRow(resolution: resolution)
                    }
                } header: {
                    Text("Sensitivity steps")
                } footer: {
                    Text("The mouse's DPI button switches between these steps. The active step is the one in use now; the default is used when the profile is selected.")
                        .foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
        }
    }
}

private struct ResolutionRow: View {
    @ObservedObject var resolution: Resolution
    @State private var draft: Double = 0
    @State private var dragging = false

    private var range: ClosedRange<Double> {
        let lo = Double(resolution.dpiList.first ?? 100)
        let hi = Double(resolution.dpiList.last ?? 16000)
        return lo ... max(lo + 1, hi)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Text("Step \(resolution.index + 1)")
                    .font(.headline)
                if resolution.isActive { Tag(text: "Active", prominent: true) }
                if resolution.isDefault { Tag(text: "Default", prominent: false) }
                if resolution.isDisabled { Tag(text: "Off", prominent: false) }
                Spacer()
                DPIField(resolution: resolution)
                Menu {
                    Button("Use Now") { resolution.makeActive() }
                        .disabled(resolution.isActive || resolution.isDisabled)
                    Button("Make Default") { resolution.makeDefault() }
                        .disabled(resolution.isDefault || resolution.isDisabled)
                    if resolution.canDisable {
                        Divider()
                        Button(resolution.isDisabled ? "Turn On" : "Turn Off") {
                            resolution.setDisabled(!resolution.isDisabled)
                        }
                        .disabled(resolution.isActive && !resolution.isDisabled)
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
            }
            if !resolution.separateXY {
                Slider(value: Binding(
                    get: { dragging ? draft : Double(resolution.dpiX) },
                    set: { draft = $0 }
                ), in: range, onEditingChanged: { editing in
                    dragging = editing
                    if !editing { resolution.setDPI(Int(draft.rounded())) }
                })
                .controlSize(.small)
            }
        }
        .padding(.vertical, 4)
        .opacity(resolution.isDisabled ? 0.5 : 1)
    }
}

private struct DPIField: View {
    @ObservedObject var resolution: Resolution

    var body: some View {
        HStack(spacing: 4) {
            if resolution.separateXY {
                number(resolution.dpiX) { resolution.setDPI(x: $0, y: resolution.dpiY) }
                Text("×").foregroundStyle(.secondary)
                number(resolution.dpiY) { resolution.setDPI(x: resolution.dpiX, y: $0) }
            } else {
                number(resolution.dpiX) { resolution.setDPI($0) }
                Stepper("", onIncrement: { step(+1) }, onDecrement: { step(-1) })
                    .labelsHidden()
            }
            Text("DPI").foregroundStyle(.secondary)
        }
    }

    private func number(_ value: Int, set: @escaping (Int) -> Void) -> some View {
        TextField("DPI", value: Binding(get: { value }, set: { set($0) }), format: .number.grouping(.never))
            .multilineTextAlignment(.trailing)
            .frame(width: 64)
            .textFieldStyle(.roundedBorder)
    }

    private func step(_ direction: Int) {
        let list = resolution.dpiList
        guard let i = list.firstIndex(where: { $0 >= resolution.dpiX }) else { return }
        let current = list[i] == resolution.dpiX ? i : (direction > 0 ? i - 1 : i)
        let next = min(max(current + direction, 0), list.count - 1)
        resolution.setDPI(list[next])
    }
}

struct Tag: View {
    let text: String
    let prominent: Bool

    var body: some View {
        Text(text)
            .font(.caption.weight(.medium))
            .padding(.horizontal, 6)
            .padding(.vertical, 1)
            .background(Capsule().fill(prominent ? Color.accentColor : Color.secondary.opacity(0.2)))
            .foregroundStyle(prominent ? Color.white : Color.secondary)
    }
}
