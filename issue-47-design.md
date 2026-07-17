# Issue 47 design: guide-synchronized loop slots

## Status and scope

This document records the agreed design for issue #47 before implementation.
It supersedes issue #5's free-running relationship between loop slots without
changing configuration schema 7, the master GoF State pattern, the one-pending-
take rule, or the dedicated worker owned by each slot.

The ordinal first configured slot is the guide. Every later slot is regular and
depends on the active guide. Concrete role assignment happens once while the
slot group is constructed. Runtime coordination uses the common `LoopSlot`
interface; it must not use subtype tests, `dynamic_cast`, or guide-specific
accessors.

## User-visible contract

- The guide must be looping before a regular slot can be armed.
- Recording the guide retains first-note time zero. Its completion-control time
  defines its period and the logical start of its first playback cycle.
- A regular slot captures the guide-relative phase of its first recording note.
- A regular take may span any number of guide cycles.
- Its repeat period is the smallest positive whole-number multiple of the guide
  period that contains all recorded musical content.
- Completing a regular take schedules its first playback at the first occurrence
  of its captured guide phase at or after completion. The completion-control
  time does not become a free-running period.
- Every later cycle is derived from the immutable absolute schedule. A late
  worker wake-up may delay one dispatch but cannot move future deadlines.
- Trailing time between the final released note and the completion control is
  discarded. Notes still held at completion end at the completion time and are
  represented by matching Note Off events in the committed take.
- Stopping a regular slot affects only that slot. Stopping the guide stops and
  discards every dependent slot first, then the guide.
- Re-recording the guide establishes a new synchronization timeline; old
  dependent takes cannot resume.

## Ownership

```text
Application
  | MIDI/control adaptation, master FSM output, live route, reporting
  v
LoopSlotGroup : LoopSlotGroupOutput
  | ordered aggregate, raw-key lookup, guide invariant, pending take
  +-- GuideLoopSlot : LoopSlot
  +-- RegularLoopSlot : LoopSlot ...

LoopSlot
  | one identity, channel, route, immutable take, playback FSM, worker
  v
SynthEngine
```

`Application` talks to `LoopSlotGroup`, never to an owned slot. The group must
not return slot references or pointers. Slot IDs, selection outcomes, MIDI
results, and immutable timing values are valid boundary values.

`LoopSlotGroup` is the aggregate entry point. It owns the ordered slots,
raw-key index, the sole bounded pending take, and cross-slot invariants. It
creates `GuideLoopSlot` for the first configured definition and
`RegularLoopSlot` for every later definition. It may retain the guide's base-
class pointer or stable index, then obtains its active schedule only through a
common `LoopSlot` operation.

`LoopSlot` retains every slot-local mechanism currently owned by the concrete
class. Only genuine role decisions are virtual. Monitoring, transposition,
activation, deactivation, termination, and worker dispatch remain common
nonvirtual behavior.

## Command-oriented slot behavior

The slot API follows Tell, Don't Ask. The group tells the selected slot that a
selection was requested; it does not query role predicates and reconstruct a
decision table.

The common base owns the playback-state part of selection:

```text
selection requested
  Looping -> derived onLoopingSelection performs its stop behavior
  Muted   -> derived onMutedSelection performs or rejects arming
```

The command returns a typed outcome only so the aggregate and master FSM can
maintain their own state and report what already happened. A command result is
not an ask-then-act protocol.

- `GuideLoopSlot` arms without a guide schedule. When selected while Looping,
  it asks the group output to stop dependents, then deactivates itself.
- `RegularLoopSlot` rejects arming when selection context has no active guide
  schedule. Otherwise it snapshots that immutable schedule as prepared-take
  metadata and arms through the common mechanism. When selected while Looping,
  it deactivates itself directly.
- Recording completion is also a command. The group finalizes its pending take
  and tells the selected slot to complete recording. The derived slot constructs
  the correct schedule; the common base commits, silences the monitored route,
  and activates playback.

The narrow cross-slot output alphabet is:

```cpp
class LoopSlotGroupOutput {
public:
    virtual ~LoopSlotGroupOutput() = default;
    virtual void stopDependentSlots() = 0;
};
```

Regular slots stop their own workers. Only the guide requests an effect outside
its boundary. The guide never sees the group implementation, its containers, or
peer slots.

## Timing values

An immutable committed take contains its events and:

```cpp
struct LoopPlaybackSchedule {
    TimePoint first_cycle_at;
    Milliseconds period;
};
```

The schedule component is a small pure domain component shared by the group and
slots. It provides guide and regular schedule construction without accessing
threads, MIDI, FluidSynth, or mutable slot state.

For guide recording start `R`, completion `C`, and positive period `T = C - R`:

```text
guide.first_cycle_at = C
guide.period         = T
```

For a regular take whose first note was at `R`, completion control was at `C`,
content ended at offset `L`, and guide schedule has origin `G` and period `T`:

```text
phase = (R - G) modulo T
first_cycle_at = first G + phase + n*T that is >= C
period = max(T, ceil(L/T) * T)
```

An event at take offset `E` in repetition `n` has absolute deadline:

```text
first_cycle_at + n*period + E
```

The worker advances from scheduled deadlines, never from `LooperClock::now()`
after activation or from its actual wake time.

## Pending take and held notes

The group-owned `PendingTake` encapsulates:

- the existing pre-reserved event vector;
- a fixed 128-key set for accepted Note Ons that remain held;
- the latest matching note-completion offset.

The armed regular slot owns the immutable guide schedule snapshot captured by
its role-specific arming command. This keeps pending event construction generic
while placing synchronization metadata with the slot that will use it to build
the committed playback schedule.

Recording preserves the hard event-count bound and makes no performance-path
allocation. The invariant `event_count + held_key_count <= maximum_event_count`
reserves closure capacity for every accepted held key. If a new Note On cannot
preserve that invariant, it is monitored but not added to the take or held set.
Matching accepted Note Offs consume their reserved capacity.

At completion, one Note Off at the completion offset is appended for every held
recorded key, then the held set is cleared. If no key is held, the content end
remains the latest completed recorded note, so delay before the completion
control is excluded. The slot's monitored channel is silenced when the take is
committed, resetting sustain and preventing a later physical release from
leaving a stuck slot-channel note.

## Concurrency and lifecycle constraints

- Master control stimuli remain serialized by `LooperFsm`.
- A slot must not hold its command mutex while calling `LoopSlotGroupOutput`;
  guide cascade would otherwise re-enter its own deactivation path.
- The group stops dependents before the guide.
- Workers never invoke role virtual methods. Role dispatch exists only on the
  control path.
- The virtual base destructor invokes only common nonvirtual termination. It
  must not dispatch role behavior.
- The group output outlives its slots because it owns them.
- Each worker retains generation-based interruption, `wait_until`, immutable
  take snapshots, and per-channel silencing.
- The base constructor's worker remains dormant until post-construction
  activation and must use only fully constructed base-owned state. If role
  behavior ever becomes necessary on the worker path, playback must first be
  extracted into a nonpolymorphic component.

## Deliberately rejected alternatives

- A central playback scheduler rewrites the dedicated-worker architecture and
  couples unrelated slot channels and lifecycles.
- A mutable guide clock shared by all workers adds synchronization and invalid
  state that immutable per-take schedules do not need.
- `GuideLoopSlot : RegularLoopSlot` makes the guide inherit dependent defaults
  even though all three role policies differ. Sibling final implementations are
  exhaustive and safer.
- Query predicates such as `roleCanArm()` leak role decisions back to the
  caller. Commands perform behavior and return only completed outcomes.
- Giving the guide the group or peer collection violates slot encapsulation.
- Routing a regular slot's self-deactivation through the group output creates an
  unnecessary callback and re-entry path.
- A guide-specific interface, downcast, capability registration, or published
  duplicate guide clock is unnecessary. The group knows the guide relationship
  once at construction and later reads the common active-schedule value.

## Verification strategy

- Deterministic examples cover exact schedule boundaries, first-note phase,
  delayed completion, multi-guide phrases, and held-note completion.
- Hegel properties apply only to the pure schedule component. Valuable
  properties are: a regular period is a positive guide multiple covering the
  content; first playback is not before completion; and first playback plus all
  repetitions preserve the captured guide phase.
- Master FSM mock tests cover selection-command outcomes and the consolidated
  recording-completion output command in every differing state.
- Current-behavior integration tests cover guide-first gating, guide stop
  cascade, independent regular stopping/re-recording, clean held-note
  completion, guide continuation during dependent recording, and shutdown.
- Thread scheduling and wall-clock behavior remain deterministic/example tests,
  not property tests.
- Run the complete normal and trace suites, `git diff --check`, clang-tidy, and
  the GCC analyzer before submission.
