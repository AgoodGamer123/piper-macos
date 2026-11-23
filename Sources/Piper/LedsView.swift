// SPDX-License-Identifier: GPL-2.0-or-later

import AppKit
import SwiftUI

struct LedsView: View {
    @ObservedObject var device: Device
    @ObservedObject var profile: Profile
    @State private var hovered: Int?

    var body: some View {
        DevicePane(model: device.model, highlighted: hovered.map { ["led\($0)"] } ?? []) {
            Form {
                ForEach(profile.leds) { led in
                    LedSection(led: led)
                        .onHover { inside in
                            if inside { hovered = led.index } else if hovered == led.index { hovered = nil }
                        }
                }
            }
            .formStyle(.grouped)
        }
    }
}

private struct LedSection: View {
    @ObservedObject var led: Led

    var body: some View {
        Section("Light \(led.index + 1)") {
            Picker("Effect", selection: Binding(get: { led.mode }, set: { led.setMode($0) })) {
                ForEach(led.modes) { Text($0.label).tag($0) }
            }
            if led.hasColor, led.mode == .solid || led.mode == .breathing {
                ColorPicker("Color", selection: colorBinding, supportsOpacity: false)
            }
            if led.mode != .off {
                LabeledContent("Brightness") {
                    Slider(value: Binding(get: { Double(led.brightness) }, set: { led.setBrightness(Int($0)) }),
                           in: 0 ... 255)
                        .frame(maxWidth: 240)
                }
            }
            if led.mode == .cycle || led.mode == .breathing {
                LabeledContent("Speed") {
                    HStack {
                        Slider(value: Binding(get: { Double(led.effectDuration) },
                                              set: { led.setEffectDuration(Int($0 / 100) * 100) }),
                               in: 100 ... 10000)
                            .frame(maxWidth: 240)
                        Text(String(format: "%.1f s", Double(led.effectDuration) / 1000))
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                            .frame(width: 44, alignment: .trailing)
                    }
                }
            }
        }
    }

    private var colorBinding: Binding<Color> {
        Binding(
            get: {
                Color(.sRGB, red: Double(led.color.red) / 255, green: Double(led.color.green) / 255,
                      blue: Double(led.color.blue) / 255, opacity: 1)
            },
            set: { color in
                guard let c = NSColor(color).usingColorSpace(.sRGB) else { return }
                led.setColor(RGB(red: Int((c.redComponent * 255).rounded()),
                                 green: Int((c.greenComponent * 255).rounded()),
                                 blue: Int((c.blueComponent * 255).rounded())))
            }
        )
    }
}
