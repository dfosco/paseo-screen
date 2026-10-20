// Draws assets/icon.png: a monitor symbol on a rounded square. Usage: swift scripts/make-icon.swift assets/icon.png
import AppKit

let size = 512.0
let image = NSImage(size: NSSize(width: size, height: size))
image.lockFocus()
let rect = NSRect(x: 0, y: 0, width: size, height: size)
let background = NSBezierPath(roundedRect: rect.insetBy(dx: 16, dy: 16), xRadius: 108, yRadius: 108)
NSGradient(starting: NSColor(calibratedRed: 0.20, green: 0.42, blue: 0.95, alpha: 1),
           ending: NSColor(calibratedRed: 0.10, green: 0.20, blue: 0.55, alpha: 1))!.draw(in: background, angle: -90)
let config = NSImage.SymbolConfiguration(pointSize: 250, weight: .regular)
  .applying(NSImage.SymbolConfiguration(paletteColors: [.white]))
if let symbol = NSImage(systemSymbolName: "display", accessibilityDescription: nil)?.withSymbolConfiguration(config) {
  let symbolSize = symbol.size
  symbol.draw(in: NSRect(x: (size - symbolSize.width) / 2, y: (size - symbolSize.height) / 2 + 6,
                         width: symbolSize.width, height: symbolSize.height))
}
image.unlockFocus()
let rep = NSBitmapImageRep(data: image.tiffRepresentation!)!
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
