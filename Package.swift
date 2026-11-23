// swift-tools-version:5.9
//
// Piper for macOS: a SwiftUI app on top of libratbag, which is compiled
// here as a C target from the vendored, ported copy in libratbag/.

import PackageDescription

// The portable libratbag build: the same files as its meson build with
// -Dhid-backend=portable (keep in sync with libratbag/meson.build)
let libratbagSources = [
    "src/asus.c",
    "src/hidpp-generic.c",
    "src/hidpp10.c",
    "src/hidpp20.c",
    "src/driver-asus.c",
    "src/driver-etekcity.c",
    "src/driver-hidpp20.c",
    "src/driver-hidpp10.c",
    "src/driver-logitech-g300.c",
    "src/driver-logitech-g600.c",
    "src/driver-roccat.c",
    "src/driver-roccat-kone-emp.c",
    "src/driver-roccat-kone-pure.c",
    "src/driver-gskill.c",
    "src/driver-openinput.c",
    "src/driver-steelseries.c",
    "src/driver-sinowealth.c",
    "src/driver-sinowealth-nubwo.c",
    "src/driver-marsgaming/driver-marsgaming.c",
    "src/driver-marsgaming/marsgaming-buttons.c",
    "src/driver-marsgaming/marsgaming-command.c",
    "src/driver-marsgaming/marsgaming-commit.c",
    "src/driver-marsgaming/marsgaming-leds.c",
    "src/driver-marsgaming/marsgaming-probe.c",
    "src/driver-marsgaming/marsgaming-query.c",
    "src/driver-test.c",
    "src/libratbag.c",
    "src/libratbag-data.c",
    "src/libratbag-hidraw.c",
    "src/libratbag-hid-portable.c",
    "src/libratbag-hid-emu.c",
    "src/libratbag-hid-iokit.c",
    "src/libratbag-test.c",
    "src/libratbag-util.c",
    "src/compat-keyfile.c",
]

let package = Package(
    name: "Piper",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "Piper", targets: ["Piper"]),
    ],
    targets: [
        .target(
            name: "CRatbag",
            path: "libratbag",
            sources: libratbagSources,
            publicHeadersPath: "swift/include",
            cSettings: [
                .define("RATBAG_PORTABLE_HID", to: "1"),
                .headerSearchPath("swift/config"),
                .headerSearchPath("src/compat"),
                .headerSearchPath("src"),
            ],
            linkerSettings: [
                .linkedFramework("IOKit"),
                .linkedFramework("CoreFoundation"),
                .linkedLibrary("iconv"),
            ]
        ),
        .executableTarget(
            name: "Piper",
            dependencies: ["CRatbag"],
            path: "Sources/Piper",
            linkerSettings: [
                .linkedFramework("WebKit"),
            ]
        ),
    ]
)
