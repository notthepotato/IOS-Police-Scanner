import AVFoundation

/// Speaks alerts the way a navigation app does: music ducks, the voice plays through
/// CarPlay / car Bluetooth, then music comes back.
@MainActor
final class Speaker: NSObject, AVSpeechSynthesizerDelegate {
    var enabled = true

    private let synthesizer = AVSpeechSynthesizer()
    private let session = AVAudioSession.sharedInstance()

    override init() {
        super.init()
        synthesizer.delegate = self
        try? session.setCategory(.playback, mode: .voicePrompt,
                                 options: [.duckOthers, .interruptSpokenAudioAndMixWithOthers])
    }

    func say(_ text: String, force: Bool = false) {
        guard enabled || force else { return }
        try? session.setActive(true)
        let utterance = AVSpeechUtterance(string: text)
        utterance.rate = AVSpeechUtteranceDefaultSpeechRate
        synthesizer.speak(utterance)
    }

    nonisolated func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance) {
        Task { @MainActor in self.releaseAudioIfIdle() }
    }

    nonisolated func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didCancel utterance: AVSpeechUtterance) {
        Task { @MainActor in self.releaseAudioIfIdle() }
    }

    private func releaseAudioIfIdle() {
        guard !synthesizer.isSpeaking else { return }
        try? session.setActive(false, options: .notifyOthersOnDeactivation)
    }
}
