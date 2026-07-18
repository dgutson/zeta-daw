# Property-based testing with Hegel

Zeta uses [Hegel](https://github.com/hegeldev/hegel-cpp) selectively for
property-based testing (PBT) of pure, deterministic contracts. Hegel generates
many inputs for one general property and shrinks a failure to a smaller
counterexample. This complements deterministic GoogleTest examples; it does
not replace them or make Hegel the default choice for every test.

The Hegel C++ version, matching native `libhegel` version, and archive hash in
`CMakeLists.txt` are authoritative. The native version cache entry is forced so
reusing a build directory after a Hegel upgrade cannot retain an
ABI-incompatible engine. Hegel is a beta test-only dependency and must not be
linked to the production `zd` CMake target or used on a real-time path. Do not
upgrade it opportunistically.

## When Hegel is a good fit

Use Hegel when all of these are true:

- The behavior is already specified by source documentation, an agreed
  requirement, existing tests, or caller expectations. PBT must not decide an
  unresolved product contract.
- The subject is deterministic and cheap to construct and call. Pure value
  transformations and dependency-free components are the strongest
  candidates.
- The input space has meaningful combinations or boundaries that a few
  examples would sample poorly.
- A non-trivial, falsifiable property can be stated independently of the
  implementation.
- A simple model, reference implementation, invariant, round trip, or
  consistency relationship can serve as the oracle.
- A minimized counterexample would be actionable to a developer.

Good Zeta patterns include:

- comparing a transformation with a simpler integer or linear-search model;
- checking that a sequence of operations preserves an agreed invariant;
- checking that related APIs agree over their shared finite domain;
- checking field preservation, bounds, idempotence, or round trips;
- checking parser robustness once the valid and invalid input contracts are
  explicit.

Prefer deterministic unit or integration tests when the assertion concerns an
exact output or error string, setup dominates the property, or behavior
depends on audio, hardware, thread scheduling, wall-clock timing, or process
lifecycle. Keep the master looper FSM and worker scheduling in deterministic
tests. A small dependency-free subordinate FSM may use Hegel's native stateful
API when it has a genuinely independent model. Hegel C++ v0.7.4 supplies named
rules, invariants checked before the first rule and after each successful rule,
sequence shrinking, and replay; this does not make wall-clock or thread
interleavings deterministic.

If no strong property is apparent after reading the implementation, existing
tests, and usage sites, do not force PBT onto the component.

## Designing a useful property

Start from evidence in the owning code and tests. State one property per Hegel
test, and make sure a plausible defect could falsify it. Prefer an independent
model or structural invariant; do not calculate the expected result by calling
the implementation under test.

Generate the broadest domain promised by the contract. Bounds are appropriate
when they define legal input, such as MIDI channels `0..15` and data bytes
`0..127`; they are not appropriate merely to avoid difficult edge cases.
Construct related valid inputs directly instead of rejecting many cases with
`tc.assume()` or generator filters. Include empty collections unless the
contract excludes them, and use unique collection generation when duplicate
keys would make the oracle ambiguous.

Avoid overflow or undefined behavior in the test model itself. When the
subject accepts an RNG, use Hegel's random generator instead of generating a
seed for another RNG; otherwise Hegel cannot shrink the individual random
decisions.

Retain deterministic tests that document important examples, regressions, and
named boundaries. The property supplies breadth; the examples communicate why
specific cases matter.

## Adding a stateful Hegel test

Use native stateful testing only for a deterministic component whose commands
and independently modeled state form one cohesive contract. Derive a test-only
machine from `hegel::stateful::StateMachine<T>`, return labeled actions from
`rules()`, and return named predicates from `invariants()`. Run the machine
inside a stable `HEGEL_TEST` so minimized rule sequences use the same failure
database and reproduction workflow as other properties:

```cpp
class ComponentMachine
    : public hegel::stateful::StateMachine<ComponentMachine> {
public:
    std::vector<hegel::stateful::Rule<ComponentMachine>> rules();
    std::vector<hegel::stateful::Invariant<ComponentMachine>> invariants();

private:
    Component subject_;
    IndependentModel model_;
};

HEGEL_TEST(component_commands_match_model)(hegel::TestCase& tc) {
    ComponentMachine machine;
    hegel::stateful::run(machine, tc);
}
```

Rules may draw their own arguments from the supplied `TestCase`. Check rule
preconditions with `tc.assume()` before mutating the machine; prefer rules that
are valid in every model state when no precondition is required. Keep the
subject and model in the test-only machine, and make invariants throw on a
contract violation so Hegel can shrink and report the labeled sequence.

Native statefulness changes test generation, not the production architecture.
Do not add a controllable clock, scheduler interface, synchronization hook, or
other production seam merely to drive a worker from Hegel without a separately
approved design.

## Adding a Hegel test

Add the property to the existing test file for the owning component. Do not
create a separate Hegel-only test file or directory. Include Hegel and give its
generators a short namespace alias:

```cpp
#include <hegel/hegel.h>

#include <stdexcept>

namespace gs = hegel::generators;
```

Define the property with `HEGEL_TEST`, then invoke that generated function from
a GoogleTest case whose suite name ends in `PropertyTest`:

```cpp
HEGEL_TEST(component_matches_reference_model)(hegel::TestCase& tc) {
    const int input = tc.draw(gs::integers<int>());

    const auto expected = referenceModel(input);
    const auto actual = componentUnderTest(input);
    if (actual != expected) {
        throw std::runtime_error("component disagrees with reference model");
    }
}

TEST(ComponentPropertyTest, MatchesReferenceModel) {
    component_matches_reference_model();
}
```

The `HEGEL_TEST` name gives Hegel a stable failure-database key. The GoogleTest
wrapper makes the property discoverable by the existing CTest integration and
by the `PropertyTest` selection convention.

Signal property failure by throwing an exception from the Hegel body, as the
existing properties do. Hegel must observe the failure so it can shrink the
generated inputs before the exception reaches GoogleTest. Use `tc.note()` for
diagnostic context that should appear with the final counterexample.

If the owning test executable does not already use Hegel, link `hegel` only to
that target inside `BUILD_TESTING`:

```cmake
target_link_libraries(component_tests PRIVATE GTest::gtest_main hegel)
```

Do not add Hegel to unrelated test targets or to the production executable.

Current examples are in:

- `tests/octave_transposer_test.cpp` for a sequence/reference model and field
  preservation;
- `tests/midi_control_change_mapping_test.cpp` for a linear-search oracle;
- `tests/midi_event_test.cpp` for the channel-message data-byte invariant;
- `tests/configuration_test.cpp` for symmetry and finite-domain consistency.
- `tests/loop_slot_fsm_test.cpp` for arbitrary subordinate playback-FSM command
  sequences compared with an independent native stateful three-state model.

## Running Hegel properties

Hegel properties are part of the normal test build and CI suite. Configure and
build them with the complete test suite:

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON
cmake --build build --parallel
```

Run every test, including all Hegel properties:

```bash
ctest --test-dir build --output-on-failure
```

Run only Hegel properties through the project naming convention:

```bash
ctest --test-dir build -R PropertyTest --output-on-failure
```

Run one property by its exact CTest name:

```bash
ctest --test-dir build \
    -R '^OctaveTransposerPropertyTest.SequenceMatchesClampedModel$' \
    --output-on-failure
```

For additional local exploration, repeat the property selection. Each local
Hegel invocation normally explores new cases:

```bash
ctest --test-dir build \
    -R PropertyTest \
    --repeat until-fail:20 \
    --output-on-failure
```

Hegel stores local counterexamples under `.hegel/`, which is ignored by Git.
Properties defined with `HEGEL_TEST` replay stored failures before generating
new examples. CI automatically disables the local database and uses
deterministic generation. Never commit `.hegel/` contents.

When a property fails, first decide whether the counterexample exposes a real
contract violation, an unsound property, or an input outside the agreed
domain. Do not narrow the generator merely to make the failure disappear. Once
a real defect is understood, keep a small deterministic regression example
when it adds useful documentation, then fix the subject in the owning issue.

## Reviewing a proposed Hegel test

Before accepting a property, confirm that:

- its requirement and input domain are explicit;
- its oracle is independent enough to detect a plausible implementation bug;
- it tests one meaningful property rather than several unrelated assertions;
- its generators include boundaries and do not reject most cases;
- failure is observable by Hegel and produces an actionable counterexample;
- deterministic examples with documentary value remain in place;
- runtime is suitable for the normal CI suite;
- dependency and CMake changes are limited to the owning test target.

Keep Hegel only while its properties remain clearer and more effective than
equivalent hand-written loops and its beta dependency does not create
disproportionate maintenance or CI friction.
