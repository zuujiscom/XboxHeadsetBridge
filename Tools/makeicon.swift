import AppKit
import CoreGraphics

// Renders the menu bar app's icon: a headset with a boom mic on a dark
// squircle. Drawn in a 1024pt design space and rendered natively at each
// output size, so nothing is upscaled. At 16/32pt the mic and the cup
// detailing are dropped — they turn to mud at that scale.
//
// Regenerate after editing:
//   swift Tools/makeicon.swift build/XboxHeadsetMenu.iconset
//   iconutil -c icns build/XboxHeadsetMenu.iconset
//
// Not part of any target — it has top-level code and builds standalone.

let outDir = CommandLine.arguments[1]
try? FileManager.default.createDirectory(
    atPath: outDir, withIntermediateDirectories: true
)

let srgb = CGColorSpace(name: CGColorSpace.sRGB)!

func color(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: srgb, components: [r, g, b, a])!
}

let xboxGreen = color(0.42, 0.83, 0.28)
let xboxGreenDim = color(0.24, 0.60, 0.20)
let plateTop = color(0.13, 0.15, 0.16)
let plateBottom = color(0.05, 0.07, 0.06)

func render(size: Int) -> CGImage? {
    let scale = CGFloat(size) / 1024.0
    func p(_ v: CGFloat) -> CGFloat { v * scale }
    let detailed = size >= 64

    guard let ctx = CGContext(
        data: nil, width: size, height: size,
        bitsPerComponent: 8, bytesPerRow: 0, space: srgb,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
    ) else { return nil }

    ctx.setAllowsAntialiasing(true)
    ctx.interpolationQuality = .high

    // macOS icon grid: an 824pt rounded square centred in a 1024pt canvas.
    let inset: CGFloat = 100
    let plate = CGRect(x: p(inset), y: p(inset), width: p(824), height: p(824))
    let platePath = CGPath(
        roundedRect: plate, cornerWidth: p(185), cornerHeight: p(185), transform: nil
    )

    ctx.saveGState()
    ctx.addPath(platePath)
    ctx.clip()
    if let gradient = CGGradient(
        colorsSpace: srgb, colors: [plateTop, plateBottom] as CFArray, locations: [0, 1]
    ) {
        ctx.drawLinearGradient(
            gradient,
            start: CGPoint(x: plate.midX, y: plate.maxY),
            end: CGPoint(x: plate.midX, y: plate.minY),
            options: []
        )
    }
    ctx.restoreGState()

    // Headset, centred slightly high so the boom has room below.
    let cx = p(512)
    let cy = p(560)
    let bandRadius = p(215)
    let bandWidth = p(74)

    ctx.setLineCap(.round)
    ctx.setStrokeColor(xboxGreen)
    ctx.setLineWidth(bandWidth)
    ctx.addArc(
        center: CGPoint(x: cx, y: cy),
        radius: bandRadius,
        startAngle: 0, endAngle: .pi,
        clockwise: false
    )
    ctx.strokePath()

    // Ear cups.
    let cupWidth = p(150)
    let cupHeight = p(235)
    for side in [-1.0, 1.0] as [CGFloat] {
        let rect = CGRect(
            x: cx + side * bandRadius - cupWidth / 2,
            y: cy - cupHeight * 0.72,
            width: cupWidth, height: cupHeight
        )
        ctx.setFillColor(xboxGreen)
        ctx.addPath(CGPath(
            roundedRect: rect, cornerWidth: p(58), cornerHeight: p(58), transform: nil
        ))
        ctx.fillPath()

        if detailed {
            let innerRect = rect.insetBy(dx: p(34), dy: p(52))
            ctx.setFillColor(xboxGreenDim)
            ctx.addPath(CGPath(
                roundedRect: innerRect, cornerWidth: p(30), cornerHeight: p(30), transform: nil
            ))
            ctx.fillPath()
        }
    }

    // Boom mic, sweeping down and in from the left cup.
    if detailed {
        ctx.setStrokeColor(xboxGreen)
        ctx.setLineWidth(p(40))
        ctx.move(to: CGPoint(x: cx - bandRadius, y: cy - p(150)))
        ctx.addCurve(
            to: CGPoint(x: cx - p(80), y: cy - p(330)),
            control1: CGPoint(x: cx - bandRadius, y: cy - p(290)),
            control2: CGPoint(x: cx - p(230), y: cy - p(345))
        )
        ctx.strokePath()

        ctx.setFillColor(xboxGreen)
        ctx.addEllipse(in: CGRect(
            x: cx - p(120), y: cy - p(372), width: p(84), height: p(84)
        ))
        ctx.fillPath()
    }

    return ctx.makeImage()
}

// iconutil expects these exact names.
let variants: [(Int, [String])] = [
    (16, ["icon_16x16"]),
    (32, ["icon_16x16@2x", "icon_32x32"]),
    (64, ["icon_32x32@2x"]),
    (128, ["icon_128x128"]),
    (256, ["icon_128x128@2x", "icon_256x256"]),
    (512, ["icon_256x256@2x", "icon_512x512"]),
    (1024, ["icon_512x512@2x"]),
]

for (size, names) in variants {
    guard let image = render(size: size) else {
        FileHandle.standardError.write("failed to render \(size)\n".data(using: .utf8)!)
        exit(1)
    }
    let rep = NSBitmapImageRep(cgImage: image)
    rep.size = NSSize(width: size, height: size)
    guard let data = rep.representation(using: .png, properties: [:]) else { exit(1) }
    for name in names {
        let url = URL(fileURLWithPath: outDir).appendingPathComponent("\(name).png")
        try! data.write(to: url)
    }
}
