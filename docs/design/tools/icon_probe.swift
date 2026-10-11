// Asks macOS for a file's icon and saves what it draws at each requested pixel size.
// usage: swift icon_probe.swift <outDir> <path.app> <size> [<size> ...]
// Same method as the audit's probe/render.swift: NSWorkspace.icon(forFile:) drawn
// into a bitmap of exactly that many pixels.
//
// The first request for a bundle the system has never seen can come back as a
// generic dashed placeholder (or a scaled-up smaller rendition) while icon
// services render the real thing in the background. So each size is requested
// repeatedly, half a second apart, until two consecutive renders are identical.
import AppKit
let args = CommandLine.arguments
let outDir = args[1], path = args[2]

func render(_ px: Int) -> (Data, NSBitmapImageRep) {
    let img = NSWorkspace.shared.icon(forFile: path)
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    rep.size = NSSize(width: px, height: px)
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    NSGraphicsContext.current!.imageInterpolation = .high
    img.draw(in: NSRect(x: 0, y: 0, width: px, height: px), from: .zero, operation: .copy, fraction: 1.0)
    NSGraphicsContext.restoreGraphicsState()
    return (Data(bytes: rep.bitmapData!, count: rep.bytesPerRow * px), rep)
}

let sizes = args.dropFirst(3).compactMap { Int($0) }
for px in sizes { _ = render(px) }          // warm-up: make the system start rendering every size
Thread.sleep(forTimeInterval: 2.0)
for px in sizes {
    var (prev, rep) = render(px)
    var tries = 1, stable = false
    while tries < 12 && !stable {
        Thread.sleep(forTimeInterval: 0.5)
        let (cur, r) = render(px)
        stable = (cur == prev)
        prev = cur; rep = r; tries += 1
    }
    let url = URL(fileURLWithPath: outDir).appendingPathComponent("system_\(px).png")
    try! rep.representation(using: .png, properties: [:])!.write(to: url)
    print("system_\(px).png  requests: \(tries)  settled: \(stable)")
}
