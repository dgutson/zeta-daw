# Loop-slot classes

How the loop-slot classes own and call each other. Each box lists the class's
constructors, public and protected methods, and private overrides;
`SynthEngine` shows only the methods these classes call. Each file's
responsibilities are in [CONTRIBUTING.md](CONTRIBUTING.md), and the
[runtime sequence diagram](runtime-sequence.puml) shows the calls in order.

```mermaid
classDiagram
    class LoopSlotGroup {
        +LoopSlotGroup(definitions, synth_engine)
        +channel(slot) int
        +monitorMidi(slot, message) int
        +requestSelection(key, soundfont, transposer) LoopSlotSelectionResult
        +cancelRecording(slot)
        +completeRecording(slot, timing)
        +terminateAll()
        +selectSoundFont(slot, soundfont)
        +octaveDown(slot)
        +octaveUp(slot)
        +recordNote(slot, kind, message, offset)
        -stopDependentSlots()
    }

    class LoopSlotGroupOutput {
        <<interface>>
        +stopDependentSlots()*
    }

    class LoopSlot {
        <<abstract>>
        #LoopSlot(id, definition, synth_engine, pending_take)
        +id() SlotId
        +selectionKey() int
        +channel() int
        +activeSchedule() optional~LoopPlaybackSchedule~
        +selectionRequested(context) LoopSlotSelectionOutcome
        +cancelRecording()
        +recordNote(kind, message, offset)
        +recordingCompleted(timing)
        +selectSoundFont(soundfont)
        +octaveDown()
        +octaveUp()
        +monitorMidi(message) int
        +deactivate()
        +terminationRequested()
        #onMutedSelection(context) LoopSlotSelectionOutcome*
        #onLoopingSelection()*
        #makeSchedule(timing, content_duration, guide) LoopPlaybackSchedule*
        #arm(context, guide) LoopSlotSelectionOutcome
    }

    class GuideLoopSlot {
        +GuideLoopSlot(id, definition, synth_engine, pending_take, output)
        -onMutedSelection(context) LoopSlotSelectionOutcome
        -onLoopingSelection()
        -makeSchedule(timing, content_duration, guide) LoopPlaybackSchedule
    }

    class RegularLoopSlot {
        +RegularLoopSlot(id, definition, synth_engine, pending_take)
        -onMutedSelection(context) LoopSlotSelectionOutcome
        -onLoopingSelection()
        -makeSchedule(timing, content_duration, guide) LoopPlaybackSchedule
    }

    class LoopSlotPlaybackFsm {
        +LoopSlotPlaybackFsm(output)
        +startRequested() LoopSlotPlaybackState
        +muteRequested() LoopSlotPlaybackState
        +terminationRequested() LoopSlotPlaybackState
        +state() LoopSlotPlaybackState
    }

    class LoopSlotPlaybackOutput {
        <<interface>>
        +activatePlayback()*
        +deactivatePlayback()*
        +terminatePlayback()*
    }

    class TakePlayer {
        +TakePlayer(synth_engine, id, channel)
        +schedule() optional~LoopPlaybackSchedule~
        +commitTake(events, schedule)
        +invalidateAndSilence()
        -activatePlayback()
        -deactivatePlayback()
        -terminatePlayback()
    }

    class MidiTakeRecorder {
        +MidiTakeRecorder(pending_take, synth_engine, channel)
        +discardTake()
        +record(kind, message, offset)
        +finishTake(timing) FinishedTake
        +selectProgram(soundfont)
    }

    class FinishedTake {
        <<struct>>
        +events
        +content_duration
    }

    class PendingTake {
        +PendingTake()
        +reset()
        +record(kind, message, offset)
        +finish(completion_offset)
        +events() vector~RecordedLoopEvent~
        +contentDuration() Milliseconds
    }

    class OctaveTransposer {
        +octaveDown()
        +octaveUp()
        +transpose(message) MidiMessage
    }

    class SynthEngine {
        +select(soundfont, channel)
        +send(message, channel) int
        +noteOn(channel, key, velocity) int
        +noteOff(channel, key) int
        +allNotesOff(channel)
    }

    LoopSlotGroupOutput <|.. LoopSlotGroup
    LoopSlotGroup "1" *-- "1..*" LoopSlot : slots, guide first
    LoopSlotGroup *-- PendingTake : pending_take, outlives the slots
    LoopSlot <|-- GuideLoopSlot
    LoopSlot <|-- RegularLoopSlot
    GuideLoopSlot --> LoopSlotGroupOutput : stops dependent slots
    LoopSlot *-- TakePlayer : player_
    LoopSlot *-- MidiTakeRecorder : recorder_
    MidiTakeRecorder --> PendingTake : records into, finishes, resets
    MidiTakeRecorder ..> FinishedTake : returns
    LoopSlot *-- OctaveTransposer : transposer_
    LoopSlot *-- LoopSlotPlaybackFsm : playback_fsm_
    LoopSlotPlaybackFsm --> LoopSlotPlaybackOutput : calls, bound to player_
    LoopSlotPlaybackOutput <|.. TakePlayer
    LoopSlot --> SynthEngine : selects SoundFont, monitors MIDI
    TakePlayer --> SynthEngine : plays notes, silences channel
    MidiTakeRecorder --> SynthEngine : selects locked SoundFont

    note for TakePlayer "public methods: called only by LoopSlot<br>private overrides: called only by LoopSlotPlaybackFsm"
```
