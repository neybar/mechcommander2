import CoreGraphics
import Foundation
// sendkeys <combo> — combo: save|load|f5|f8|esc
let combos: [String: (CGKeyCode, CGEventFlags)] = [
    "save": (7, [.maskControl, .maskAlternate, .maskShift]),  // ctrl+alt+shift+X
    "load": (6, [.maskControl, .maskAlternate, .maskShift]),  // ctrl+alt+shift+Z
    "f5":   (96, []),
    "f8":   (100, []),
    "esc":  (53, []),
]
guard CommandLine.arguments.count > 1, let (code, flags) = combos[CommandLine.arguments[1]] else {
    print("usage: sendkeys save|load|f5|f8|esc"); exit(1)
}
// press modifier keys first so SDL tracks mod state
let mods: [(CGEventFlags, CGKeyCode)] = [(.maskControl, 59), (.maskAlternate, 58), (.maskShift, 56)]
for (f, mc) in mods where flags.contains(f) {
    CGEvent(keyboardEventSource: nil, virtualKey: mc, keyDown: true)?.post(tap: .cghidEventTap)
    usleep(30000)
}
let down = CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: true)
down?.flags = flags
down?.post(tap: .cghidEventTap)
usleep(80000)
let up = CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: false)
up?.flags = flags
up?.post(tap: .cghidEventTap)
usleep(30000)
for (f, mc) in mods.reversed() where flags.contains(f) {
    CGEvent(keyboardEventSource: nil, virtualKey: mc, keyDown: false)?.post(tap: .cghidEventTap)
    usleep(30000)
}
