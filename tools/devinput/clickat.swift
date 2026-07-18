import CoreGraphics
import Foundation
// clickat x y  (desktop points)
guard CommandLine.arguments.count > 2,
      let x = Double(CommandLine.arguments[1]), let y = Double(CommandLine.arguments[2]) else {
    print("usage: clickat x y"); exit(1)
}
let p = CGPoint(x: x, y: y)
CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
usleep(120000)
CGEvent(mouseEventSource: nil, mouseType: .leftMouseDown, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
usleep(90000)
CGEvent(mouseEventSource: nil, mouseType: .leftMouseUp, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
