// SPDX-License-Identifier: GPL-2.0-or-later

import AppKit
import SwiftUI

struct ButtonsView: View {
    @ObservedObject var device: Device
    @ObservedObject var profile: Profile
    @State private var hovered: Int?

    var body: some View {
        DevicePane(model: device.model, highlighted: hovered.map { ["button\($0)"] } ?? []) {
            Form {
                Section {
                    ForEach(profile.buttons) { button in
                        ButtonRow(button: button, buttonCount: device.numButtons)
                            .onHover { inside in
                                if inside { hovered = button.index } else if hovered == button.index { hovered = nil }
                            }
                    }
                } header: {
                    Text("Buttons")
                } footer: {
                    Text("Hover over a button to see where it is on the mouse.")
                        .foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
        }
    }
}

private struct ButtonRow: View {
    @ObservedObject var button: MouseButton
    let buttonCount: Int
    @State private var recording: KeyRecorder.Mode?

    var body: some View {
        LabeledContent("Button \(button.index + 1)") {
            Menu {
                menuItems
            } label: {
                Text(ActionText.describe(button.action))
                    .lineLimit(1)
            }
            .fixedSize()
        }
        .sheet(item: $recording) { mode in
            KeyRecorderSheet(mode: mode) { events in
                apply(recorded: events, mode: mode)
            }
        }
    }

    @ViewBuilder private var menuItems: some View {
        if button.supportedKinds.contains(.button) {
            Section("Mouse Buttons") {
                ForEach(1 ... max(buttonCount, 5), id: \.self) { n in
                    choice(ActionText.mouseButton(n), .button(n))
                }
            }
        }
        if button.supportedKinds.contains(.special) {
            Section("Special") {
                ForEach(SpecialAction.allCases) { special in
                    choice(special.label, .special(special))
                }
            }
        }
        if button.supportedKinds.contains(.key) || button.supportedKinds.contains(.macro) {
            Section("Keyboard") {
                if button.supportedKinds.contains(.key) {
                    Button("Keystroke…") { recording = .keystroke }
                }
                if button.supportedKinds.contains(.macro) {
                    Button("Macro…") { recording = .macro }
                }
            }
        }
        if button.supportedKinds.contains(.disabled) {
            Divider()
            choice("Disabled", .disabled)
        }
    }

    private func choice(_ title: String, _ action: ButtonAction) -> some View {
        Button {
            button.setAction(action)
        } label: {
            if button.action == action {
                Label(title, systemImage: "checkmark")
            } else {
                Text(title)
            }
        }
    }

    private func apply(recorded events: [MacroEvent], mode: KeyRecorder.Mode) {
        guard !events.isEmpty else { return }
        // a single key press and release becomes a key action when possible
        if events.count == 2, events[0].kind == .press, events[1].kind == .release,
           events[0].value == events[1].value, button.supportedKinds.contains(.key)
        {
            button.setAction(.key(events[0].value))
        } else if button.supportedKinds.contains(.macro) {
            button.setAction(.macro(events))
        } else if let key = events.first(where: { $0.kind == .press && !modifierEvdevCodes.contains($0.value) })
            ?? events.first(where: { $0.kind == .press })
        {
            // no macros on this button: keep the main key of a shortcut
            button.setAction(.key(key.value))
        }
    }
}

/// Linux key codes of the modifier keys
let modifierEvdevCodes: Set<Int> = [29, 97, 42, 54, 56, 100, 125, 126, 58]

enum ActionText {
    static func mouseButton(_ n: Int) -> String {
        switch n {
        case 1: return "Left Click"
        case 2: return "Right Click"
        case 3: return "Middle Click"
        case 4: return "Back"
        case 5: return "Forward"
        default: return "Button \(n)"
        }
    }

    static func key(_ code: Int) -> String {
        evdevKeys[UInt32(code)]?.label ?? "Key \(code)"
    }

    static func describe(_ action: ButtonAction) -> String {
        switch action {
        case .disabled: return "Disabled"
        case let .button(n): return mouseButton(n)
        case let .special(s): return s.label
        case let .key(code): return "Key: \(key(code))"
        case let .macro(events): return "Macro: " + macro(events)
        case .unknown: return "Unknown"
        }
    }

    /// "⌘ C" style for a press/release pair, ↓/↑ otherwise, like Piper
    static func macro(_ events: [MacroEvent]) -> String {
        var parts: [String] = []
        var i = 0
        while i < events.count {
            let e = events[i]
            switch e.kind {
            case .wait:
                parts.append("\(e.value) ms")
            case .press:
                if i + 1 < events.count, events[i + 1].kind == .release, events[i + 1].value == e.value {
                    parts.append(key(e.value))
                    i += 1
                } else {
                    parts.append("↓\(key(e.value))")
                }
            case .release:
                parts.append("↑\(key(e.value))")
            }
            i += 1
        }
        return parts.isEmpty ? "Empty" : parts.joined(separator: " ")
    }
}

// MARK: - Recording keystrokes

enum KeyRecorder {
    enum Mode: String, Identifiable {
        case keystroke, macro
        var id: String { rawValue }
    }
}

private struct KeyRecorderSheet: View {
    let mode: KeyRecorder.Mode
    let onDone: ([MacroEvent]) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var events: [MacroEvent] = []
    @State private var monitor: Any?
    @State private var pressedModifiers = Set<UInt16>()
    @State private var unknownKey = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(mode == .keystroke ? "Record a Keystroke" : "Record a Macro")
                .font(.headline)
            Text(mode == .keystroke
                ? "Press the key the mouse button should send. Hold modifiers like ⌘ or ⇧ to record a shortcut."
                : "Type the keys the mouse button should send, in order. Click Done when finished.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Text(events.isEmpty ? "Waiting for keys…" : ActionText.macro(events))
                .font(.system(.title3, design: .rounded))
                .frame(maxWidth: .infinity, minHeight: 56)
                .background(RoundedRectangle(cornerRadius: 8).fill(Color.secondary.opacity(0.12)))

            if unknownKey {
                Text("That key has no equivalent the mouse can send and was skipped.")
                    .font(.callout).foregroundStyle(.orange)
            }

            HStack {
                Button("Clear") {
                    events = []
                    unknownKey = false
                }
                .disabled(events.isEmpty)
                Spacer()
                Button("Cancel") { dismiss() }
                Button("Done") {
                    onDone(events)
                    dismiss()
                }
                .buttonStyle(.borderedProminent)
                .disabled(events.isEmpty)
            }
        }
        .padding(20)
        .frame(width: 420)
        .onAppear(perform: startMonitoring)
        .onDisappear(perform: stopMonitoring)
    }

    private func startMonitoring() {
        // Every key goes into the recording, Return and Esc included: the
        // sheet's buttons are meant to be clicked
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { event in
            record(event)
            return nil
        }
    }

    private func stopMonitoring() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
    }

    private func record(_ event: NSEvent) {
        if event.type == .keyDown, event.isARepeat { return }
        guard let code = macToEvdev[event.keyCode] else {
            if event.type != .flagsChanged { unknownKey = true }
            return
        }
        let value = Int(code)
        switch event.type {
        case .keyDown:
            events.append(MacroEvent(kind: .press, value: value))
        case .keyUp:
            events.append(MacroEvent(kind: .release, value: value))
        case .flagsChanged:
            // a modifier went down if we hadn't seen it down before
            if pressedModifiers.contains(event.keyCode) {
                pressedModifiers.remove(event.keyCode)
                events.append(MacroEvent(kind: .release, value: value))
            } else {
                pressedModifiers.insert(event.keyCode)
                events.append(MacroEvent(kind: .press, value: value))
            }
        default:
            return
        }

        // a keystroke is complete once everything is released again
        if mode == .keystroke, event.type != .keyDown, pressedModifiers.isEmpty,
           events.contains(where: { $0.kind == .press && !modifierEvdevCodes.contains($0.value) })
        {
            onDone(events)
            dismiss()
        }
    }
}
