// SPDX-License-Identifier: GPL-2.0-or-later
//
// The device picture: Piper's SVG for the device, drawn by WebKit so the
// selected button or LED can be highlighted by its id ("button3", "led0").

import AppKit
import SwiftUI
import WebKit

enum DeviceArt {
    /// svg-lookup.ini: [Name] DeviceMatch=usb:046d:4086;... Svg=file.svg
    private static let lookup: [String: String] = {
        guard let url = svgDirectory?.appendingPathComponent("svg-lookup.ini"),
              let text = try? String(contentsOf: url, encoding: .utf8) else { return [:] }
        var table: [String: String] = [:]
        var matches: [String] = []
        var svg: String?
        func flush() {
            if let svg { for m in matches { table[m] = svg } }
            matches = []
            svg = nil
        }
        for line in text.split(whereSeparator: \.isNewline) {
            let l = line.trimmingCharacters(in: .whitespaces)
            if l.hasPrefix("[") {
                flush()
            } else if l.hasPrefix("DeviceMatch=") {
                matches = l.dropFirst("DeviceMatch=".count).split(separator: ";").map(String.init)
            } else if l.hasPrefix("Svg=") {
                svg = String(l.dropFirst("Svg=".count))
            }
        }
        flush()
        return table
    }()

    static var svgDirectory: URL? {
        Bundle.main.resourceURL?.appendingPathComponent("svgs")
    }

    /// The SVG for a model string like "usb:046d:4086:0"
    static func svgURL(for model: String) -> URL? {
        let parts = model.split(separator: ":")
        var key = model
        if parts.count == 4, parts[3] == "0" {
            key = parts.prefix(3).joined(separator: ":")
        }
        let file = lookup[key] ?? "fallback.svg"
        return svgDirectory?.appendingPathComponent(file)
    }
}

struct DeviceImageView: NSViewRepresentable {
    let model: String
    /// Element ids to highlight, e.g. ["button3"]
    var highlighted: [String] = []

    final class Coordinator: NSObject, WKNavigationDelegate {
        var loadedModel: String?
        var loaded = false
        var pendingHighlight: [String] = []

        func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
            loaded = true
            DeviceImageView.applyHighlight(pendingHighlight, in: webView)
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.suppressesIncrementalRendering = true
        let view = WKWebView(frame: .zero, configuration: config)
        view.navigationDelegate = context.coordinator
        view.setValue(false, forKey: "drawsBackground")
        view.allowsMagnification = false
        return view
    }

    func updateNSView(_ view: WKWebView, context: Context) {
        let coordinator = context.coordinator
        coordinator.pendingHighlight = highlighted
        if coordinator.loadedModel != model {
            coordinator.loadedModel = model
            coordinator.loaded = false
            view.loadHTMLString(Self.html(for: model), baseURL: nil)
        } else if coordinator.loaded {
            Self.applyHighlight(highlighted, in: view)
        }
    }

    private static func html(for model: String) -> String {
        let svg = DeviceArt.svgURL(for: model)
            .flatMap { try? String(contentsOf: $0, encoding: .utf8) } ?? ""
        // Drop the XML prolog; the SVG goes inline so CSS and JS reach it
        let body = svg.range(of: "<svg").map { String(svg[$0.lowerBound...]) } ?? ""
        return """
        <!DOCTYPE html>
        <html><head><meta charset="utf-8"><style>
        html, body { margin: 0; height: 100%; background: transparent; overflow: hidden; }
        body { display: flex; align-items: center; justify-content: center; }
        svg { width: 100%; height: 100%; }
        /* Piper's leader lines and labels are drawn natively instead */
        #Buttons, #LEDs { display: none !important; }
        .piper-highlight { fill: #0a84ff !important; fill-opacity: 0.85 !important;
                           stroke: #0a84ff !important; transition: fill 0.15s; }
        @media (prefers-color-scheme: dark) {
          .piper-highlight { fill: #409cff !important; stroke: #409cff !important; }
        }
        </style></head><body>\(body)</body></html>
        """
    }

    fileprivate static func applyHighlight(_ ids: [String], in view: WKWebView) {
        let list = ids.map { "'\($0)'" }.joined(separator: ",")
        let js = """
        document.querySelectorAll('.piper-highlight').forEach(e => e.classList.remove('piper-highlight'));
        [\(list)].forEach(id => { const e = document.getElementById(id); if (e) e.classList.add('piper-highlight'); });
        """
        view.evaluateJavaScript(js, completionHandler: nil)
    }
}
