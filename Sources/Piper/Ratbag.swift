// SPDX-License-Identifier: GPL-2.0-or-later
//
// Swift models on top of libratbag (the CRatbag module).
//
// Values are cached in @Published properties and re-read from libratbag
// after every change, so SwiftUI never touches the C library while a commit
// runs on a background queue. All other libratbag calls happen on the main
// thread.

import CRatbag
import Foundation

// MARK: - Helpers

/// Converts between C enums imported by Swift (whose raw type depends on
/// the enum's values) and plain Ints.
@inline(__always) private func raw<T: RawRepresentable>(_ value: T) -> Int where T.RawValue: BinaryInteger {
    Int(value.rawValue)
}

@inline(__always) private func cenum<T: RawRepresentable>(_ value: Int) -> T where T.RawValue: BinaryInteger {
    T(rawValue: T.RawValue(truncatingIfNeeded: value))!
}

private func uintList(_ fill: (UnsafeMutablePointer<UInt32>, Int) -> Int, capacity: Int = 64) -> [Int] {
    var buffer = [UInt32](repeating: 0, count: capacity)
    let count = buffer.withUnsafeMutableBufferPointer { fill($0.baseAddress!, capacity) }
    return buffer.prefix(min(count, capacity)).map { Int($0) }
}

struct RatbagError: LocalizedError {
    let code: Int

    var errorDescription: String? {
        switch code {
        case -1000: return "The device did not accept the change, or communication with it failed."
        case -1001: return "The device does not support this."
        case -1002: return "The value is out of range for this device."
        case -1003: return "A system error occurred while talking to the device."
        case -1004: return "Internal error in libratbag."
        default: return "Error \(code)."
        }
    }
}

// MARK: - Context

/// The libratbag context: finds devices and keeps the device list current.
final class RatbagContext: ObservableObject {
    @Published private(set) var devices: [Device] = []
    @Published private(set) var hasScanned = false

    private let ptr: OpaquePointer
    private var groups: [String: [Device]] = [:]
    /// Groups without a usable device (a receiver whose mouse sleeps, a
    /// keyboard libratbag doesn't know): polls left until the next try,
    /// and the current back-off interval
    private var retry: [String: (left: Int, interval: Int)] = [:]
    private var timer: Timer?
    fileprivate var busyCount = 0

    static let pollInterval: TimeInterval = 2

    init?() {
        // The .device files ship inside the app
        if ProcessInfo.processInfo.environment["LIBRATBAG_DATA_DIR"] == nil,
           let dir = Bundle.main.resourceURL?.appendingPathComponent("devices"),
           FileManager.default.fileExists(atPath: dir.path)
        {
            setenv("LIBRATBAG_DATA_DIR", dir.path, 1)
        }

        guard let ctx = ratbag_create_context_default() else { return nil }
        ptr = ctx

        switch ProcessInfo.processInfo.environment["PIPER_RATBAG_DEBUG"] {
        case "raw": ratbag_log_set_priority(ctx, RATBAG_LOG_PRIORITY_RAW)
        case .some: ratbag_log_set_priority(ctx, RATBAG_LOG_PRIORITY_DEBUG)
        case .none: ratbag_log_set_priority(ctx, RATBAG_LOG_PRIORITY_ERROR)
        }
    }

    func start() {
        guard timer == nil else { return }
        poll()
        hasScanned = true
        timer = Timer.scheduledTimer(withTimeInterval: Self.pollInterval, repeats: true) { [weak self] _ in
            self?.poll()
        }
    }

    private func listGroups() -> [String] {
        guard let raw = ratbag_hid_list_groups(ptr) else { return [] }
        defer { ratbag_free_string(raw) }
        return String(cString: raw)
            .split(separator: "\n")
            .compactMap { $0.split(separator: "\t", omittingEmptySubsequences: false).first.map(String.init) }
    }

    private func probe(group: String) -> [Device] {
        var pointers = [OpaquePointer?](repeating: nil, count: 8)
        let count = Int(ratbag_devices_new_from_group(ptr, group, &pointers, Int32(pointers.count)))
        return pointers.prefix(count).compactMap { $0 }.map { Device(ptr: $0, context: self) }
    }

    /// Notices plugged and unplugged devices
    func poll() {
        // never while a commit talks to a device
        guard busyCount == 0 else { return }

        let present = Set(listGroups())
        var list = devices

        for (group, groupDevices) in groups where !present.contains(group) {
            for device in groupDevices {
                device.isConnected = false
            }
            list.removeAll { d in groupDevices.contains { $0 === d } }
            groups[group] = nil
            retry[group] = nil
        }

        for group in present.sorted() {
            if let known = groups[group], !known.isEmpty { continue }
            if var r = retry[group] {
                r.left -= 1
                retry[group] = r
                if r.left > 0 { continue }
            }
            let found = probe(group: group)
            groups[group] = found
            if found.isEmpty {
                let interval = min((retry[group]?.interval ?? 1) * 2, 30)
                retry[group] = (interval, interval)
            } else {
                retry[group] = nil
                list.append(contentsOf: found)
            }
        }

        if list.map(\.id) != devices.map(\.id) {
            devices = list
        }
    }

    deinit {
        timer?.invalidate()
        ratbag_unref(ptr)
    }
}

// MARK: - Device

final class Device: ObservableObject, Identifiable, Hashable {
    let id: String
    let name: String
    /// "usb:046d:4086:0", like ratbagd's Model property
    let model: String
    let profiles: [Profile]
    let numButtons: Int
    let numLeds: Int
    let firmwareVersion: String

    @Published var isConnected = true
    @Published private(set) var isDirty = false
    @Published private(set) var isCommitting = false
    @Published var lastError: String?

    fileprivate let ptr: OpaquePointer
    private weak var context: RatbagContext?

    fileprivate init(ptr: OpaquePointer, context: RatbagContext) {
        self.ptr = ptr
        self.context = context

        let group = ratbag_device_get_group(ptr).map { String(cString: $0) } ?? UUID().uuidString
        id = "\(group)#\(ratbag_device_get_receiver_index(ptr))"
        name = ratbag_device_get_name(ptr).map { String(cString: $0) } ?? "Unknown device"
        firmwareVersion = ratbag_device_get_firmware_version(ptr).map { String(cString: $0) } ?? ""
        if let bus = ratbag_device_get_bustype(ptr) {
            model = String(format: "%@:%04x:%04x:%d", String(cString: bus),
                           ratbag_device_get_vendor_id(ptr), ratbag_device_get_product_id(ptr),
                           ratbag_device_get_product_version(ptr))
        } else {
            model = "unknown"
        }
        numButtons = Int(ratbag_device_get_num_buttons(ptr))
        numLeds = Int(ratbag_device_get_num_leds(ptr))

        var profiles: [Profile] = []
        for i in 0 ..< Int(ratbag_device_get_num_profiles(ptr)) {
            if let p = ratbag_device_get_profile(ptr, UInt32(i)) {
                profiles.append(Profile(ptr: p, index: i))
            }
        }
        self.profiles = profiles
        for profile in profiles {
            profile.device = self
            profile.loadChildren()
        }
        refreshDirty()
    }

    var activeProfile: Profile? { profiles.first { $0.isActive } ?? profiles.first }

    /// Whether changes are allowed right now
    var isEditable: Bool { isConnected && !isCommitting }

    fileprivate func refreshDirty() {
        let dirty = profiles.contains { $0.isDirty }
        if dirty != isDirty { isDirty = dirty }
    }

    fileprivate func report(_ code: Int) {
        if code < 0 { lastError = RatbagError(code: code).errorDescription }
    }

    /// Writes all changes to the device's memory, off the main thread
    func commit() {
        guard isEditable, isDirty, let context else { return }
        isCommitting = true
        context.busyCount += 1
        let devicePtr = ptr
        DispatchQueue.global(qos: .userInitiated).async {
            let rc = Int(ratbag_device_commit(devicePtr).rawValue)
            DispatchQueue.main.async {
                context.busyCount -= 1
                self.isCommitting = false
                if rc != 0 {
                    self.lastError = "Could not write the changes to \(self.name). "
                        + (RatbagError(code: rc).errorDescription ?? "")
                }
                for profile in self.profiles { profile.reloadAll() }
                self.refreshDirty()
            }
        }
    }

    static func == (a: Device, b: Device) -> Bool { a === b }
    func hash(into hasher: inout Hasher) { hasher.combine(ObjectIdentifier(self)) }

    deinit {
        // children first: destroying the device frees them in libratbag
        for profile in profiles { profile.release() }
        ratbag_device_unref(ptr)
    }
}

// MARK: - Profile

final class Profile: ObservableObject, Identifiable {
    let index: Int
    var id: Int { index }
    fileprivate(set) unowned var device: Device!

    @Published private(set) var name = ""
    @Published private(set) var isActive = false
    @Published private(set) var isEnabled = true
    @Published private(set) var isDirty = false
    @Published private(set) var reportRate = 0
    @Published private(set) var angleSnapping = -1
    @Published private(set) var debounce = -1

    let reportRates: [Int]
    let debounces: [Int]
    let canDisable: Bool
    private(set) var resolutions: [Resolution] = []
    private(set) var buttons: [MouseButton] = []
    private(set) var leds: [Led] = []

    fileprivate let ptr: OpaquePointer

    fileprivate init(ptr: OpaquePointer, index: Int) {
        self.ptr = ptr
        self.index = index
        reportRates = uintList { ratbag_profile_get_report_rate_list(ptr, $0, $1) }
        debounces = uintList { ratbag_profile_get_debounce_list(ptr, $0, $1) }
        canDisable = ratbag_profile_has_capability(ptr, RATBAG_PROFILE_CAP_DISABLE)
        reload()
    }

    fileprivate func loadChildren() {
        resolutions = (0 ..< Int(ratbag_profile_get_num_resolutions(ptr))).compactMap { i in
            ratbag_profile_get_resolution(ptr, UInt32(i)).map { Resolution(ptr: $0, index: i, profile: self) }
        }
        buttons = (0 ..< device.numButtons).compactMap { i in
            ratbag_profile_get_button(ptr, UInt32(i)).map { MouseButton(ptr: $0, index: i, profile: self) }
        }
        leds = (0 ..< device.numLeds).compactMap { i in
            ratbag_profile_get_led(ptr, UInt32(i)).map { Led(ptr: $0, index: i, profile: self) }
        }
    }

    var displayName: String {
        name.isEmpty ? "Profile \(index + 1)" : name
    }

    var supportsAngleSnapping: Bool { angleSnapping >= 0 }
    var supportsDebounce: Bool { debounce >= 0 && !debounces.isEmpty }
    var supportsReportRate: Bool { reportRate > 0 && !reportRates.isEmpty }

    fileprivate func reload() {
        name = ratbag_profile_get_name(ptr).map { String(cString: $0) } ?? ""
        isActive = ratbag_profile_is_active(ptr)
        isEnabled = ratbag_profile_is_enabled(ptr)
        isDirty = ratbag_profile_is_dirty(ptr)
        reportRate = Int(ratbag_profile_get_report_rate(ptr))
        angleSnapping = Int(ratbag_profile_get_angle_snapping(ptr))
        debounce = Int(ratbag_profile_get_debounce(ptr))
    }

    fileprivate func reloadAll() {
        reload()
        resolutions.forEach { $0.reload() }
        buttons.forEach { $0.reload() }
        leds.forEach { $0.reload() }
    }

    /// Re-reads after a change made to this profile or one of its children
    fileprivate func changed(_ code: Int) {
        device.report(code)
        reload()
        device.refreshDirty()
    }

    fileprivate func release() {
        resolutions.forEach { ratbag_resolution_unref($0.ptr) }
        buttons.forEach { ratbag_button_unref($0.ptr) }
        leds.forEach { ratbag_led_unref($0.ptr) }
        ratbag_profile_unref(ptr)
    }

    // MARK: Changes

    func setName(_ name: String) {
        changed(Int(ratbag_profile_set_name(ptr, name)))
    }

    func setEnabled(_ enabled: Bool) {
        changed(raw(ratbag_profile_set_enabled(ptr, enabled)))
    }

    func makeActive() {
        let rc = raw(ratbag_profile_set_active(ptr))
        device.report(rc)
        for profile in device.profiles { profile.reload() }
        device.refreshDirty()
    }

    func setReportRate(_ hz: Int) {
        changed(raw(ratbag_profile_set_report_rate(ptr, UInt32(hz))))
    }

    func setAngleSnapping(_ on: Bool) {
        changed(raw(ratbag_profile_set_angle_snapping(ptr, on ? 1 : 0)))
    }

    func setDebounce(_ ms: Int) {
        changed(raw(ratbag_profile_set_debounce(ptr, Int32(ms))))
    }
}

// MARK: - Resolution

final class Resolution: ObservableObject, Identifiable {
    let index: Int
    var id: Int { index }
    unowned let profile: Profile

    /// Every DPI value the sensor supports, ascending
    let dpiList: [Int]
    let separateXY: Bool
    let canDisable: Bool

    @Published private(set) var dpiX = 0
    @Published private(set) var dpiY = 0
    @Published private(set) var isActive = false
    @Published private(set) var isDefault = false
    @Published private(set) var isDisabled = false

    fileprivate let ptr: OpaquePointer

    fileprivate init(ptr: OpaquePointer, index: Int, profile: Profile) {
        self.ptr = ptr
        self.index = index
        self.profile = profile
        dpiList = uintList({ ratbag_resolution_get_dpi_list(ptr, $0, $1) }, capacity: 1024).sorted()
        separateXY = ratbag_resolution_has_capability(ptr, RATBAG_RESOLUTION_CAP_SEPARATE_XY_RESOLUTION)
        canDisable = ratbag_resolution_has_capability(ptr, RATBAG_RESOLUTION_CAP_DISABLE)
        reload()
    }

    fileprivate func reload() {
        dpiX = Int(ratbag_resolution_get_dpi_x(ptr))
        dpiY = Int(ratbag_resolution_get_dpi_y(ptr))
        isActive = ratbag_resolution_is_active(ptr)
        isDefault = ratbag_resolution_is_default(ptr)
        isDisabled = ratbag_resolution_is_disabled(ptr)
    }

    /// The supported value closest to dpi
    func nearestSupported(_ dpi: Int) -> Int {
        dpiList.min { abs($0 - dpi) < abs($1 - dpi) } ?? dpi
    }

    func setDPI(_ dpi: Int) {
        let value = nearestSupported(dpi)
        let rc = raw(ratbag_resolution_set_dpi(ptr, UInt32(value)))
        reload()
        profile.changed(rc)
    }

    func setDPI(x: Int, y: Int) {
        let rc = raw(ratbag_resolution_set_dpi_xy(ptr, UInt32(nearestSupported(x)), UInt32(nearestSupported(y))))
        reload()
        profile.changed(rc)
    }

    func makeActive() {
        let rc = raw(ratbag_resolution_set_active(ptr))
        profile.resolutions.forEach { $0.reload() }
        profile.changed(rc)
    }

    func makeDefault() {
        let rc = raw(ratbag_resolution_set_default(ptr))
        profile.resolutions.forEach { $0.reload() }
        profile.changed(rc)
    }

    func setDisabled(_ disabled: Bool) {
        let rc = raw(ratbag_resolution_set_disabled(ptr, disabled))
        profile.resolutions.forEach { $0.reload() }
        profile.changed(rc)
    }
}

// MARK: - Buttons

struct MacroEvent: Hashable {
    enum Kind: Hashable { case press, release, wait }
    var kind: Kind
    /// Linux key code, or milliseconds for .wait
    var value: Int
}

enum ButtonAction: Hashable {
    case disabled
    /// Logical mouse button, 1 = left, 2 = right, 3 = middle, ...
    case button(Int)
    case special(SpecialAction)
    /// Linux key code
    case key(Int)
    case macro([MacroEvent])
    case unknown

    enum Kind: CaseIterable { case disabled, button, special, key, macro }
}

/// libratbag's special button functions
enum SpecialAction: Int, CaseIterable, Identifiable {
    case doubleClick = 1, wheelLeft, wheelRight, wheelUp, wheelDown, ratchetModeSwitch
    case resolutionCycleUp, resolutionCycleDown, resolutionUp, resolutionDown
    case resolutionAlternate, resolutionDefault
    case profileCycleUp, profileCycleDown, profileUp, profileDown
    case secondMode, batteryLevel

    var id: Int { rawValue }

    /// The libratbag value: (1 << 30) + n
    var ratbagValue: Int { (1 << 30) + rawValue }

    init?(ratbagValue: Int) {
        self.init(rawValue: ratbagValue - (1 << 30))
    }

    var label: String {
        switch self {
        case .doubleClick: return "Double Click"
        case .wheelLeft: return "Wheel Left"
        case .wheelRight: return "Wheel Right"
        case .wheelUp: return "Wheel Up"
        case .wheelDown: return "Wheel Down"
        case .ratchetModeSwitch: return "Ratchet Mode Switch"
        case .resolutionCycleUp: return "Cycle Resolution Up"
        case .resolutionCycleDown: return "Cycle Resolution Down"
        case .resolutionUp: return "Resolution Up"
        case .resolutionDown: return "Resolution Down"
        case .resolutionAlternate: return "Resolution Switch (hold)"
        case .resolutionDefault: return "Default Resolution"
        case .profileCycleUp: return "Cycle Profile Up"
        case .profileCycleDown: return "Cycle Profile Down"
        case .profileUp: return "Profile Up"
        case .profileDown: return "Profile Down"
        case .secondMode: return "Second Mode (G-Shift)"
        case .batteryLevel: return "Show Battery Level"
        }
    }
}

final class MouseButton: ObservableObject, Identifiable {
    let index: Int
    var id: Int { index }
    unowned let profile: Profile
    let supportedKinds: Set<ButtonAction.Kind>

    @Published private(set) var action: ButtonAction = .unknown

    fileprivate let ptr: OpaquePointer

    fileprivate init(ptr: OpaquePointer, index: Int, profile: Profile) {
        self.ptr = ptr
        self.index = index
        self.profile = profile
        var kinds = Set<ButtonAction.Kind>()
        let map: [(ButtonAction.Kind, ratbag_button_action_type)] = [
            (.disabled, RATBAG_BUTTON_ACTION_TYPE_NONE),
            (.button, RATBAG_BUTTON_ACTION_TYPE_BUTTON),
            (.special, RATBAG_BUTTON_ACTION_TYPE_SPECIAL),
            (.key, RATBAG_BUTTON_ACTION_TYPE_KEY),
            (.macro, RATBAG_BUTTON_ACTION_TYPE_MACRO),
        ]
        for (kind, type) in map where ratbag_button_has_action_type(ptr, type) {
            kinds.insert(kind)
        }
        supportedKinds = kinds
        reload()
    }

    fileprivate func reload() {
        let type = raw(ratbag_button_get_action_type(ptr))
        switch type {
        case raw(RATBAG_BUTTON_ACTION_TYPE_NONE):
            action = .disabled
        case raw(RATBAG_BUTTON_ACTION_TYPE_BUTTON):
            action = .button(Int(ratbag_button_get_button(ptr)))
        case raw(RATBAG_BUTTON_ACTION_TYPE_SPECIAL):
            let value = raw(ratbag_button_get_special(ptr))
            action = SpecialAction(ratbagValue: value).map { .special($0) } ?? .unknown
        case raw(RATBAG_BUTTON_ACTION_TYPE_KEY):
            action = .key(Int(ratbag_button_get_key(ptr)))
        case raw(RATBAG_BUTTON_ACTION_TYPE_MACRO):
            action = .macro(readMacro())
        default:
            action = .unknown
        }
    }

    private func readMacro() -> [MacroEvent] {
        guard let macro = ratbag_button_get_macro(ptr) else { return [] }
        defer { ratbag_button_macro_unref(macro) }
        var events: [MacroEvent] = []
        for i in 0 ..< ratbag_button_macro_get_num_events(macro) {
            let type = raw(ratbag_button_macro_get_event_type(macro, i))
            switch type {
            case raw(RATBAG_MACRO_EVENT_KEY_PRESSED):
                events.append(MacroEvent(kind: .press, value: Int(ratbag_button_macro_get_event_key(macro, i))))
            case raw(RATBAG_MACRO_EVENT_KEY_RELEASED):
                events.append(MacroEvent(kind: .release, value: Int(ratbag_button_macro_get_event_key(macro, i))))
            case raw(RATBAG_MACRO_EVENT_WAIT):
                events.append(MacroEvent(kind: .wait, value: Int(ratbag_button_macro_get_event_timeout(macro, i))))
            default:
                return events
            }
        }
        return events
    }

    func setAction(_ action: ButtonAction) {
        let rc: Int
        switch action {
        case .disabled:
            rc = raw(ratbag_button_disable(ptr))
        case let .button(n):
            rc = raw(ratbag_button_set_button(ptr, UInt32(n)))
        case let .special(special):
            rc = raw(ratbag_button_set_special(ptr, cenum(special.ratbagValue)))
        case let .key(code):
            rc = raw(ratbag_button_set_key(ptr, UInt32(code)))
        case let .macro(events):
            rc = setMacro(events)
        case .unknown:
            return
        }
        reload()
        profile.changed(rc)
    }

    private func setMacro(_ events: [MacroEvent]) -> Int {
        guard let macro = ratbag_button_macro_new("macro") else { return -1003 }
        defer { ratbag_button_macro_unref(macro) }
        for (i, event) in events.enumerated() {
            let type: ratbag_macro_event_type
            switch event.kind {
            case .press: type = RATBAG_MACRO_EVENT_KEY_PRESSED
            case .release: type = RATBAG_MACRO_EVENT_KEY_RELEASED
            case .wait: type = RATBAG_MACRO_EVENT_WAIT
            }
            let rc = raw(ratbag_button_macro_set_event(macro, UInt32(i), type, UInt32(event.value)))
            if rc != 0 { return rc }
        }
        return raw(ratbag_button_set_macro(ptr, macro))
    }
}

// MARK: - LEDs

enum LedMode: Int, CaseIterable, Identifiable {
    case off = 0, solid, cycle, breathing

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .off: return "Off"
        case .solid: return "Solid"
        case .cycle: return "Color Cycle"
        case .breathing: return "Breathing"
        }
    }
}

struct RGB: Hashable {
    var red: Int
    var green: Int
    var blue: Int
}

final class Led: ObservableObject, Identifiable {
    let index: Int
    var id: Int { index }
    unowned let profile: Profile
    let modes: [LedMode]
    /// false for single-color LEDs
    let hasColor: Bool

    @Published private(set) var mode: LedMode = .off
    @Published private(set) var color = RGB(red: 0, green: 0, blue: 0)
    @Published private(set) var brightness = 0
    @Published private(set) var effectDuration = 0

    fileprivate let ptr: OpaquePointer

    fileprivate init(ptr: OpaquePointer, index: Int, profile: Profile) {
        self.ptr = ptr
        self.index = index
        self.profile = profile
        modes = LedMode.allCases.filter { ratbag_led_has_mode(ptr, cenum($0.rawValue)) }
        hasColor = raw(ratbag_led_get_colordepth(ptr)) != raw(RATBAG_LED_COLORDEPTH_MONOCHROME)
        reload()
    }

    fileprivate func reload() {
        mode = LedMode(rawValue: raw(ratbag_led_get_mode(ptr))) ?? .off
        let c = ratbag_led_get_color(ptr)
        color = RGB(red: Int(c.red), green: Int(c.green), blue: Int(c.blue))
        brightness = Int(ratbag_led_get_brightness(ptr))
        effectDuration = Int(ratbag_led_get_effect_duration(ptr))
    }

    private func apply(_ rc: Int) {
        reload()
        profile.changed(rc)
    }

    func setMode(_ mode: LedMode) {
        apply(raw(ratbag_led_set_mode(ptr, cenum(mode.rawValue))))
    }

    func setColor(_ color: RGB) {
        let c = ratbag_color(red: UInt32(color.red), green: UInt32(color.green), blue: UInt32(color.blue))
        apply(raw(ratbag_led_set_color(ptr, c)))
    }

    func setBrightness(_ value: Int) {
        apply(raw(ratbag_led_set_brightness(ptr, UInt32(max(0, min(255, value))))))
    }

    func setEffectDuration(_ ms: Int) {
        apply(raw(ratbag_led_set_effect_duration(ptr, UInt32(max(0, min(10000, ms))))))
    }
}
