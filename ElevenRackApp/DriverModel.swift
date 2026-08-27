// DriverModel.swift — single source of truth for the UI and the status icon.
//
// Samples the ring (levels, sample rate, streaming test), the engine supervisor,
// CoreAudio (authoritative device rate for the picker) and CoreMIDI on a timer,
// and publishes a small state that both the status item and the control panel
// observe. Meters update fast; status/rate/MIDI update a few times a second.

import Foundation
import Combine
import CoreAudio

final class DriverModel: ObservableObject {
    enum Status {
        case active        // device connected, streaming
        case idleNoDevice  // engine up but Eleven Rack not connected
        case notRunning    // engine unavailable / not supervising

        var title: String {
            switch self {
            case .active: return "Active"
            case .idleNoDevice: return "Device not connected"
            case .notRunning: return "Engine not running"
            }
        }
    }

    @Published private(set) var status: Status = .notRunning
    @Published private(set) var sampleRate: UInt32 = 0
    @Published private(set) var inputLevels = [Float](repeating: 0, count: ER.inputNames.count)
    @Published private(set) var outputLevels = [Float](repeating: 0, count: ER.outputNames.count)
    @Published private(set) var midiPresent = false
    @Published private(set) var deviceAvailable = false
    /// Current Rig Input index (object 3D: 0 Guitar, 1 Re-Amp, …), or nil until
    /// the hardware first reports it. Drives the Rig Input picker.
    @Published private(set) var rigInput: Int? = nil
    /// True only when audio is genuinely dropping out: an app is actively using
    /// the device AND overruns are climbing. Suppressed while idle (where the
    /// ring saturates harmlessly with no consumer).
    @Published private(set) var dropoutWarning = false
    @Published var launchAtLogin = LaunchAgentControl.isEnabled
    /// Persisted hardware clock source (1 Internal · 2 AES · 3 S/PDIF). The engine asserts it at startup;
    /// changing it re-launches the engine so it re-applies. Only we ever change it — no polling needed.
    @Published private(set) var clockSource: Int =
        UserDefaults.standard.object(forKey: ER.clockSourceKey) as? Int ?? ER.defaultClockSource
    /// Whether the current clock source is locked to a valid signal (engine-reported; digital only).
    @Published private(set) var clockLocked = true

    /// Overruns-per-poll above which, with an active consumer, we flag dropouts
    /// (~a few ms of audio in a 0.33 s window).
    private static let xrunWarnThreshold: UInt32 = 300
    /// Consecutive warning polls required before showing the flag, so brief
    /// transients (a DAW opening the device, a sample-rate change) don't trip it.
    private static let warnStreakToShow = 3

    private let ring = RingReader()
    private unowned let engine: EngineController
    private var deviceID: AudioDeviceID?
    private var meterTimer: Timer?
    private var statusTimer: Timer?
    private var warnStreak = 0

    private let rigInputMIDI = RigInputMIDI()

    /// Whether the control panel is on screen. The 30 Hz meters only publish while
    /// it is: otherwise SwiftUI re-lays out the (closed) popover view 30×/second,
    /// burning ~30% CPU animating meters nobody can see — enough to spin the fan and
    /// starve the engine's real-time USB handling. Set by StatusController.
    var panelVisible = false

    init(engine: EngineController) {
        self.engine = engine
        // onValue is delivered on the main queue by RigInputMIDI.
        rigInputMIDI.onValue = { [weak self] value in self?.rigInput = value }
    }

    /// Begin polling. Meters at ~30 Hz, status/rate/MIDI at ~3 Hz.
    func start() {
        let mt = Timer(timeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in self?.pollMeters() }
        let st = Timer(timeInterval: 0.33, repeats: true) { [weak self] _ in self?.pollStatus() }
        RunLoop.main.add(mt, forMode: .common)
        RunLoop.main.add(st, forMode: .common)
        meterTimer = mt
        statusTimer = st
        pollStatus()
    }

    func stop() {
        meterTimer?.invalidate(); statusTimer?.invalidate()
        meterTimer = nil; statusTimer = nil
    }

    /// Change the device sample rate (drives CoreAudio, like Audio MIDI Setup).
    /// The engine's rate-follow then retunes the hardware.
    func setSampleRate(_ hz: UInt32) {
        if deviceID == nil { deviceID = CoreAudioDevice.find() }
        guard let id = deviceID else { return }
        CoreAudioDevice.setRate(hz, on: id)
    }

    /// Change the hardware clock source. Persists the choice and restarts the engine, which re-asserts it
    /// via ER_CLOCK on relaunch (a clock-master change needs a stream re-lock anyway).
    func setClockSource(_ src: Int) {
        guard src != clockSource else { return }
        clockSource = src
        UserDefaults.standard.set(src, forKey: ER.clockSourceKey)
        engine.restart()
    }

    func toggleLaunchAtLogin(_ on: Bool) {
        if on { LaunchAgentControl.enable() } else { LaunchAgentControl.disable() }
        launchAtLogin = LaunchAgentControl.isEnabled
    }

    /// Set the Eleven Rack's Rig Input source. Clamped to the valid range; the UI
    /// updates optimistically (no read-back needed).
    func setRigInput(_ index: Int) {
        let clamped = max(0, min(RigInput.maxValue, index))
        rigInput = clamped
        rigInputMIDI.write(clamped)
    }

    /// Read the current Rig Input from the hardware once (called when the popover
    /// opens). Deliberately on-demand: continuous MIDI polling during streaming
    /// starves the isochronous audio and makes the engine drop/respawn.
    func refreshRigInput() {
        guard midiPresent else { return }
        rigInputMIDI.requestRead()
    }

    // MARK: - Polling

    private func pollMeters() {
        // Nobody's looking: don't churn @Published state (and thus SwiftUI layout)
        // for a panel that isn't on screen. Resumes instantly when it reopens.
        guard panelVisible else { return }
        // Only show live levels while actually streaming. Otherwise clear the
        // meters instead of leaving the last captured peaks frozen on screen
        // (which looked like input on a disconnected device).
        guard status == .active, ring.isAttached else {
            if inputLevels.contains(where: { $0 != 0 }) {
                inputLevels = Array(repeating: 0, count: ER.inputNames.count)
            }
            if outputLevels.contains(where: { $0 != 0 }) {
                outputLevels = Array(repeating: 0, count: ER.outputNames.count)
            }
            return
        }
        inputLevels = ring.inputLevels()
        outputLevels = ring.outputLevels()
    }

    private func pollStatus() {
        ring.attachIfNeeded()

        var ringSampleRate: UInt32 = 0
        // Require engineRunning: a ring can outlive its engine (valid magic, stale
        // contents). Only when the engine reports it is running do we trust the ring
        // for Active/idle; otherwise fall through to the supervision-based state.
        if let act = ring.poll(), act.engineRunning {
            ringSampleRate = act.sampleRate
            status = act.enginePulling ? .active : .idleNoDevice
            // Overruns only matter while an app is actually pulling audio through
            // the device; when idle the ring saturates by design.
            if act.consumerActive && act.xrunDelta > Self.xrunWarnThreshold {
                warnStreak += 1
            } else {
                warnStreak = 0
            }
            dropoutWarning = warnStreak >= Self.warnStreakToShow
            clockLocked = act.clockLocked
        } else {
            status = (engine.supervising && engine.isEngineAvailable) ? .idleNoDevice : .notRunning
            inputLevels = Array(repeating: 0, count: ER.inputNames.count)
            outputLevels = Array(repeating: 0, count: ER.outputNames.count)
            dropoutWarning = false
            warnStreak = 0
            clockLocked = true       // no engine → no lock info; don't show a false alarm
        }

        // Authoritative rate + presence from CoreAudio (what AMS/DAWs show).
        if deviceID == nil { deviceID = CoreAudioDevice.find() }
        if let id = deviceID, let rate = CoreAudioDevice.currentRate(of: id) {
            deviceAvailable = true
            sampleRate = rate
        } else {
            deviceID = nil
            deviceAvailable = false
            if ringSampleRate != 0 { sampleRate = ringSampleRate }
        }

        midiPresent = MidiStatus.elevenRackPresent
        // Clear the cached value when the device goes away so the picker disables.
        // No MIDI is sent here — reads happen only on demand (refreshRigInput).
        if !midiPresent && rigInput != nil { rigInput = nil }
    }
}
