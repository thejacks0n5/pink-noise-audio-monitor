// Prints Apple's continuous-corner rounded rectangle as SVG path data (y down).
// usage: swift export_body_path.swift x y w h radius
// SwiftUI's RoundedRectangle(style: .continuous) is the shape the macOS icon
// template uses: 824x824 at (100,100), radius 185.4 on a 1024 canvas.
import SwiftUI
let a = CommandLine.arguments.dropFirst().map { Double($0)! }
let rect = CGRect(x: a[0], y: a[1], width: a[2], height: a[3])
let path = RoundedRectangle(cornerRadius: a[4], style: .continuous).path(in: rect).cgPath
var d = ""
func f(_ p: CGPoint) -> String { String(format: "%.3f,%.3f", p.x, p.y) }
path.applyWithBlock { e in
    let p = e.pointee.points
    switch e.pointee.type {
    case .moveToPoint: d += "M" + f(p[0])
    case .addLineToPoint: d += "L" + f(p[0])
    case .addQuadCurveToPoint: d += "Q" + f(p[0]) + " " + f(p[1])
    case .addCurveToPoint: d += "C" + f(p[0]) + " " + f(p[1]) + " " + f(p[2])
    case .closeSubpath: d += "Z"
    @unknown default: break
    }
}
print(d)
