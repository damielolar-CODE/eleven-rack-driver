// RigInput.swift — read and set the Eleven Rack's Rig Input over CoreMIDI SysEx.
//
// The Rig Input source (Guitar, Re-Amp, Mic, Line, Digital) lives on the Eleven
// Rack's USB editor/control interface, which the class-compliant driver surfaces
// as the "Eleven Rack Rig" CoreMIDI port. This is the one input MIDI CC can't
// reach, and it's what enables computer re-amping: set Rig Input = Re-Amp, play a
// dry track out the Re-Amp L+R outputs, and record the wet return on Eleven Rig
// L+R. The DAW does the actual re-amping; this just picks the input.
//
// Protocol (verified on hardware 2026-08-14): frame
//   F0 13 0B 0F <fn> <obj> [payload] F7
// functions 01=read, 12=response, 00=set, 02=async-ack; object 3D = Rig Input as
// a single raw byte index. Valid indices are 0...8; writing 9+ crashes the unit,
// so writes are clamped and out-of-range values are refused.

import Foundation
import CoreMIDI

/// The Rig Input enumeration for object 3D. The index order matches the hardware
/// (verified by write-sweep); index 1 is Re-Amp.
enum RigInput {
    static let names = ["Guitar", "Re-Amp", "Mic", "Line L", "Line R",
                        "Line L+R", "Digital L", "Digital R", "Digital L+R"]
    static let guitar = 0
    static let reAmp = 1
    static let maxValue = 8

    static func name(_ i: Int) -> String {
        (0...maxValue).contains(i) ? names[i] : "?"
    }
}

/// Owns a CoreMIDI client for reading and writing object 3D. Reads and write-acks
/// are delivered asynchronously via `onValue` (always on the main queue).
final class RigInputMIDI {
    /// Invoked whenever the hardware reports a Rig Input value — in reply to a
    /// read, or as the async ack to a write. Called on the main queue.
    var onValue: ((Int) -> Void)?

    private var client = MIDIClientRef()
    private var inPort = MIDIPortRef()
    private var outPort = MIDIPortRef()
    private var connectedSources = Set<MIDIEndpointRef>()

    // SysEx frame constants.
    private static let start: UInt8 = 0xF0, vendor: UInt8 = 0x13, dev: UInt8 = 0x0B
    private static let model: UInt8 = 0x0F, end: UInt8 = 0xF7
    private static let fnSet: UInt8 = 0x00, fnReq: UInt8 = 0x01
    private static let fnResp: UInt8 = 0x12, fnAck: UInt8 = 0x02
    private static let objInput: UInt8 = 0x3D

    // SysEx reassembly (a message can, in principle, span packets).
    private var rxAcc = [UInt8]()
    private var inSysex = false

    init() {
        MIDIClientCreate("Eleven Rack (rig input)" as CFString, nil, nil, &client)
        MIDIOutputPortCreate(client, "erRigIn out" as CFString, &outPort)
        MIDIInputPortCreateWithBlock(client, "erRigIn in" as CFString, &inPort) { [weak self] list, _ in
            self?.handle(list)
        }
        // Sources are connected only for the duration of a read (see requestRead),
        // never held open in the background: keeping the device's MIDI interface
        // active during streaming can disturb the isochronous audio stream.
    }

    /// (Re)connect the input port to every "Eleven Rack" source. Cheap to call
    /// repeatedly — a set guards against reconnecting the same endpoint, and new
    /// endpoints (device plugged in later) are picked up on the next call.
    func connectSources() {
        for i in 0..<MIDIGetNumberOfSources() {
            let src = MIDIGetSource(i)
            guard Self.displayName(src)?.contains(ER.deviceName) == true,
                  !connectedSources.contains(src) else { continue }
            if MIDIPortConnectSource(inPort, src, nil) == noErr { connectedSources.insert(src) }
        }
    }

    /// Ask the hardware for the current Rig Input. The value arrives via `onValue`.
    /// The MIDI input is opened just for this exchange and released ~2 s later so it
    /// isn't held open during streaming.
    func requestRead() {
        connectSources()
        send([Self.start, Self.vendor, Self.dev, Self.model, Self.fnReq, Self.objInput, Self.end])
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in
            self?.disconnectSources()
        }
    }

    private func disconnectSources() {
        for src in connectedSources { MIDIPortDisconnectSource(inPort, src) }
        connectedSources.removeAll()
    }

    /// Set the Rig Input by index. Out-of-range values are refused (9+ crashes the
    /// hardware). The hardware echoes the new value back as an ack via `onValue`.
    func write(_ index: Int) {
        guard (0...RigInput.maxValue).contains(index) else { return }
        send([Self.start, Self.vendor, Self.dev, Self.model, Self.fnSet,
              Self.objInput, UInt8(index), Self.end])
    }

    // MARK: - Internals

    /// The "Eleven Rack Rig" destination; falls back to any "Eleven Rack" dest.
    private func destination() -> MIDIEndpointRef? {
        var fallback: MIDIEndpointRef?
        for i in 0..<MIDIGetNumberOfDestinations() {
            let d = MIDIGetDestination(i)
            guard let nm = Self.displayName(d), nm.contains(ER.deviceName) else { continue }
            if nm.contains("Rig") { return d }
            if fallback == nil { fallback = d }
        }
        return fallback
    }

    private func send(_ bytes: [UInt8]) {
        guard let dest = destination() else { return }
        var list = MIDIPacketList()
        let cur = MIDIPacketListInit(&list)
        _ = MIDIPacketListAdd(&list, MemoryLayout<MIDIPacketList>.size, cur, 0, bytes.count, bytes)
        MIDISend(outPort, dest, &list)
    }

    private func handle(_ list: UnsafePointer<MIDIPacketList>) {
        var packet = list.pointee.packet
        for _ in 0..<list.pointee.numPackets {
            let len = Int(packet.length)
            withUnsafeBytes(of: &packet.data) { raw in
                for i in 0..<len {
                    let b = raw[i]
                    if b == Self.start { inSysex = true; rxAcc.removeAll(keepingCapacity: true) }
                    if inSysex { rxAcc.append(b) }
                    if b == Self.end && inSysex { inSysex = false; parse(rxAcc) }
                }
            }
            packet = MIDIPacketNext(&packet).pointee
        }
    }

    /// Parse F0 13 0B 0F <12|02> 3D <value> F7 and report the value.
    private func parse(_ msg: [UInt8]) {
        guard msg.count >= 8, msg[0] == Self.start, msg[1] == Self.vendor,
              msg[2] == Self.dev, msg[3] == Self.model,
              msg[4] == Self.fnResp || msg[4] == Self.fnAck,
              msg[5] == Self.objInput else { return }
        let value = Int(msg[6])
        DispatchQueue.main.async { [weak self] in self?.onValue?(value) }
    }

    private static func displayName(_ ep: MIDIEndpointRef) -> String? {
        var cf: Unmanaged<CFString>?
        guard MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cf) == noErr,
              let s = cf?.takeRetainedValue() as String? else { return nil }
        return s
    }
}
