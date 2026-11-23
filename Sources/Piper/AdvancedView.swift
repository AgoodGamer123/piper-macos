// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

struct AdvancedView: View {
    @ObservedObject var device: Device
    @ObservedObject var profile: Profile

    var body: some View {
        DevicePane(model: device.model) {
            Form {
                Section("Sensor") {
                    if profile.supportsReportRate {
                        Picker("Report rate", selection: Binding(get: { profile.reportRate },
                                                                  set: { profile.setReportRate($0) })) {
                            ForEach(profile.reportRates, id: \.self) { Text("\($0) Hz").tag($0) }
                        }
                        .pickerStyle(.segmented)
                    } else {
                        LabeledContent("Report rate", value: "Not adjustable")
                    }

                    if profile.supportsAngleSnapping {
                        Toggle("Angle snapping", isOn: Binding(get: { profile.angleSnapping > 0 },
                                                                set: { profile.setAngleSnapping($0) }))
                    }

                    if profile.supportsDebounce {
                        Picker("Debounce time", selection: Binding(get: { profile.debounce },
                                                                    set: { profile.setDebounce($0) })) {
                            ForEach(profile.debounces, id: \.self) { Text("\($0) ms").tag($0) }
                        }
                    }
                }

                Section("Device") {
                    LabeledContent("Name", value: device.name)
                    LabeledContent("Model", value: device.model)
                    if !device.firmwareVersion.isEmpty {
                        LabeledContent("Firmware", value: device.firmwareVersion)
                    }
                    LabeledContent("Profiles", value: "\(device.profiles.count)")
                    LabeledContent("Buttons", value: "\(device.numButtons)")
                }
            }
            .formStyle(.grouped)
        }
    }
}
