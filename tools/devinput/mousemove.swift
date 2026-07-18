import CoreGraphics
import Foundation
// sweep the mouse around the screen for N seconds: args: seconds
let secs = Double(CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "20") ?? 20
let start = Date()
var t = 0.0
while Date().timeIntervalSince(start) < secs {
    // lissajous sweep across a 2048x1080-ish desktop, stays in middle region
    let x = 1000 + 700 * cos(t * 0.9)
    let y = 550 + 350 * sin(t * 1.3)
    let ev = CGEvent(mouseEventSource: nil, mouseType: .mouseMoved,
                     mouseCursorPosition: CGPoint(x: x, y: y), mouseButton: .left)
    ev?.post(tap: .cghidEventTap)
    t += 0.05
    usleep(16000)
}
