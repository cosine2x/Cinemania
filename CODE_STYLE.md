# Cinemania C++20 style and implementation guidelines

Project convention for our C++20 source and tests. Vendored dependencies retain upstream formatting. Formatting and static-analysis settings are defined by the root .clang-format and .clang-tidy files.

The objective is code whose ownership, purpose and control flow are apparent on a first reading. Formatting is mandatory, but it cannot compensate for a module doing unrelated jobs. These are project choices informed by the C++ Core Guidelines, not claims that one brace style or naming convention is universally modern C++.

## 1. Non-negotiable readability rules

1. **Use braces for every conditional and loop body**, including a one-statement early return. Never put an executable conditional/loop body on the same line as its condition.
2. **Use Allman braces**, four spaces, no tabs, and a 100-column target. Function, class, namespace, lambda and control-flow opening braces go on their own lines. Aggregate/braced initializers are not executable block bodies and may remain inline.
3. **One statement per line and one variable declaration per statement.** Do not compress declarations, assignments or cleanup into semicolon-separated runs.
4. **Separate logical groups with one blank line.** Use a blank line between validation, resource acquisition, preparation, side effects and returning a result when these are distinct phases. Separate adjacent completed control-flow blocks from the next logical group. Do not insert a blank line between every statement inside one cohesive group.
5. **Keep one abstraction level in a function.** A capture coordinator should read like a capture operation, not alternate between protocol bytes, Windows handle plumbing, encoder arguments and UI text.
6. **Make ownership and failure visible.** No hidden ownership transfer, borrowed buffers queued asynchronously, ignored important results, or silently swallowed failures.
7. **Do not make code shorter by making it harder to inspect.** Avoid nested ternaries, clever expression tricks, side-effecting conditions, anonymous boolean arguments and dense lambdas.

Bad layout:

```cpp
if (!session) return error; auto slot = acquire(); if (!slot) return busy;
copy(slot, frame); queue(slot); return success;
```

Required shape (illustrative names, not an implemented API):

```cpp
if (!session.is_recording())
{
    return Error::invalid_state;
}

auto slot_result = frame_pool.acquire();
if (!slot_result)
{
    return slot_result.error();
}

auto slot = std::move(slot_result).value();
copy_pixels(frame, slot.bytes());

return sender.enqueue(std::move(slot));
```

The grouping here is intentional: check session state; acquire/check a resource; prepare it; transfer ownership. A formatter can preserve the blank lines, but **the author must choose these groups**.

Allow `else if` on one line as a language construct; its body still has braces on separate lines. Allow `= default` and `= delete` declarations on one line; do not expand them into meaningless empty bodies. Do not align columns manually with long runs of spaces.

## 2. Cohesion, boundaries and function size

Each module has one concrete responsibility, a small explicit interface and owned state. Boundaries include game ABI, hook installation, capture session, frame pool, IPC transport, protocol, encoder process and output publication.

- Do not create `utils.cpp`, `common_helpers.hpp` or a growing `Manager` object to absorb unrelated code. Put a helper beside the behavior that owns it; name a shared module after a real shared responsibility.
- The game ABI layer knows addresses/calling conventions; the encoder layer does not. The protocol layer knows bytes and message invariants; it does not open files or show dialogs. The session coordinates components without reaching into their internals.
- Extract a helper when it names a coherent operation, isolates unsafe/platform behavior, establishes an invariant, or removes meaningful duplication. Do not extract every two lines or invent an interface for one trivial call.
- Prefer direct composition and free functions for stateless operations. Use a class when it owns a resource or maintains a meaningful invariant. Use a struct for an ordinary data record. Avoid inheritance unless runtime substitutability is actually required.
- Keep functions focused enough to describe in one sentence without listing unrelated jobs. Around **60 substantive lines** or **more than three nested control-flow levels** triggers review, not an automatic failure. Dispatch tables/state machines may be longer when splitting them would hide the transition logic.
- Flatten validation/error paths using early returns. Do not build a deep success-path pyramid. Do not turn a readable switch into a callback registry merely to shorten the function.
- Prefer a single settings/options struct over many positional parameters, especially booleans or repeated integer types. Do not put unrelated dependencies into a universal context object just to shorten a parameter list.
- Keep filesystem, process and game effects behind the modules that own them, so their logic can be exercised without starting the game.

## 3. Naming and declarations

| Item | Convention | Example |
|---|---|---|
| Namespace | lower_snake_case | `cinemania::capture` |
| Type / concept / type alias | PascalCase | `CaptureSession`, `FrameView`, `SessionId` |
| Function / method | lower_snake_case | `begin_capture`, `publish_output` |
| Local / parameter / public data field | lower_snake_case | `frame_bytes`, `sample_count` |
| Private data member | lower_snake_case with trailing underscore | `state_`, `frame_pool_` |
| Namespace/class constant | kPascalCase | `kMaxQueuedBytes` |
| Local constant | lower_snake_case | `const auto frame_bytes = ...;` |
| Scoped enum value | lower_snake_case | `SessionState::encoding_video` |
| Source/header filename | lower_snake_case | `capture_session.cpp`, `capture_session.hpp` |

Use domain names rather than `data`, `tmp`, `ctx`, `val`, `ret`, `thing`, `process_stuff`, or `do_work`. Short names such as `i` are fine for a tiny obvious loop. Encode important units: `payload_bytes`, `frame_index`, `sample_rate_hz`. Prefer chrono duration types for timeouts rather than naked integers with undocumented units.

ABI/exported names, Win32 field names and upstream names keep their required spellings. Do not rename a calling convention into a misleading generic alias.

- Use leading const (`const FrameDescriptor&`) and attach pointer/reference symbols to the type (`FrameView* frame`).
- Declare variables near first use and initialize them. Prefer const when a value will not change. Use explicit semantic initialization rather than choosing braces mechanically if that changes overload selection.
- Use `auto` when the initializer makes the type clear, for iterators and for named factory/lookup results. Spell out ABI types, arithmetic widths and conversions where they affect correctness. Avoid both blanket `auto` use and needless repetition of long obvious types.
- Prefer explicit return types in ordinary named functions. Use trailing return types when they make a generic declaration clearer, not as a blanket style requirement.
- Mark single-argument converting constructors explicit unless implicit conversion is deliberately part of the API. Use override on overrides; use default/delete to express special-member intent.
- Put [[nodiscard]] on important status/result-returning operations and resource factories. Do not annotate every getter indiscriminately. A deliberate ignored result needs an explanation when the failure matters.
- Keep access sections grouped by purpose: public interface first, private implementation/state last. Do not mix public and private fields unpredictably. Avoid redundant boilerplate getters/setters for passive records.

## 4. C++20 features: choose the useful ones

Use standard facilities when they simplify the actual operation: span, string_view, array, vector, unique_ptr, optional, variant, scoped enums, chrono, move semantics, constexpr, scoped locks and named algorithms.

- `std::span`/`std::string_view` are borrowed views, not owners. Document the lifetime if it is not immediately obvious; do not store a view into a temporary or across an asynchronous handoff.
- `std::vector` may own preallocated frame storage. Normal capture must not resize/reallocate it. Pass a span to code that only needs access to the bytes.
- Prefer `std::unique_ptr` or concrete move-only owners; `std::shared_ptr` requires a real shared-lifetime requirement and an explanation of who participates.
- Prefer a clear loop or named algorithm over a long ranges/adaptor chain. Use lambdas locally and briefly; captures are explicit for stored/asynchronous lambdas. A lambda with its own multistage workflow deserves a named operation.
- Use templates/concepts for demonstrated reusable type relationships. Do not build a generic framework for one capture pipeline. Avoid macro-based error propagation or pseudo-language control flow.
- Use `std::size_t` for local container sizes and fixed-width integers at protocol/ABI boundaries. Validate before narrowing; static_cast is not validation. Use signed types when negative values are meaningful.
- Use distinct lightweight types for identifiers that are easy to confuse when it helps correctness. Do not wrap every integer merely to appear type-safe.
- Stay within C++20. Do not accidentally introduce `std::expected`, `std::scope_exit`, `std::print` or `std::byteswap`, which require a newer standard/library. Agree on one small result vocabulary for this project rather than adding a second result framework in each module.

## 5. Ownership, errors and cleanup

Every owned resource has one visible owner. Resource wrappers are move-only unless copying has an explicit safe meaning. Prefer the rule of zero for ordinary classes; implement special members only where ownership requires them.

- Own handles, files, pipes, buffers and child-process lifetime using RAII. Raw pointers/references normally borrow. Keep raw owning handles inside the narrow wrapper/ABI layer; make adoption versus borrowing explicit.
- Ownership transfer is visible in the signature and call, commonly through a move-only value and `std::move`. Do not move from const values or add moves that defeat return-value optimization.
- Use one `Error` representation carrying an operation/stage, an error category and the original platform/encoder code where relevant. A plain bool is appropriate for a predicate, not a complex operation with several failure modes. Optional means ordinary absence; it must not conceal an operational error.
- Expected runtime failures return an explicit result/status. Assertions express programmer invariants, not user input/worker-protocol validation. No assertion with a necessary side effect.
- Capture GetLastError immediately after the failing call, before logging, cleanup or another Windows call can replace it. Preserve HRESULT values as HRESULTs rather than confusing them with Win32 error codes.
- Catch exceptions at the foreign callback/thread entry boundary and translate them to the agreed result. Do not permit unwinding into the game or Windows. Add noexcept only when the implementation actually upholds the contract; it is not a substitute for handling allocation failure.
- Destructors must not throw. Critical actions such as encoder drain, final mux, publication and checked I/O completion happen through explicit operations with results; a destructor supplies fallback cleanup, not an invisible successful commit.
- Do not log the same failure at every layer. Lower layers add structured context; the owning boundary reports it once. Empty catch blocks and silent success after partial failure are unacceptable.
- If cleanup also fails, retain the primary failure and attach/report cleanup information appropriately. Never erase the original cause with a secondary close error.

## 6. State, concurrency and performance

- Model the capture lifecycle with a scoped state enum and explicit transitions. Avoid combinations of booleans such as started/finished/closing/error/cancelled that admit contradictory states.
- Each mutable field has a documented thread owner or synchronization rule. Group fields protected by the same lock. Do not make everything atomic as a substitute for a concurrency design; volatile is not synchronization.
- Use named scoped locks. Keep lock scopes visibly short; use a nested scope when release timing matters. Avoid file/pipe I/O, waiting on another component, logging callbacks and foreign game calls while holding a state lock unless the protocol specifically requires and documents it.
- Wait predicates include data/space readiness, cancellation and worker failure. Use deadlines and events/condition variables; do not introduce a Sleep polling loop for convenience or a busy spin without measurements.
- Use the default strongest atomic ordering until a specific weaker-order protocol has been designed and reviewed. Comment the required happens-before relationship when weaker ordering is used.
- Hook callbacks perform validated lookup, bounded copying and queue handoff. No normal per-frame allocation, string formatting, encoder startup or synchronous verbose logging. Queue-full backpressure remains intentional and interruptible.
- Record aggregated timings/counters; optimize measured costs. Avoid speculative assembly, lock-free data structures, alignment tricks or custom allocators.
- Treat a pending overlapped I/O buffer as owned by the operation until completion, including after cancellation was requested. A handle close or cancellation request is not proof that the buffer is reusable.

## 7. Low-level and hook code

Unsafe/platform-specific operations belong in small named adapters; the rest of the program should use ordinary typed operations.

- Isolate casts, memory protection changes, import-table edits and byte patches. Use explicit C++ casts and explain any reinterpret_cast that crosses a representation/lifetime boundary. Do not spread pointer arithmetic through business logic.
- Separate static RVA, runtime address and session/stream ID types/names. Resolve addresses from the validated module base; never copy a live address from research into code.
- Put each supported build's addresses/signatures/layout facts in one cohesive compatibility description, with provenance. Do not scatter unexplained offsets and magic byte sequences across hook handlers.
- Match every foreign function's exact ABI and ownership contract. Add static assertions for verified layout sizes/offsets where appropriate; assertions cannot prove an unverified reverse-engineered layout.
- Do not serialize native structs, padding, pointers, C++ containers or HANDLEs as if x86 and x64 shared a layout. Decode protocol fields explicitly and use checked size arithmetic before accessing buffers.
- Do not dereference arbitrary stale synthetic handles. Keep identity/reference checks in the registry boundary and leave policy decisions to the session layer.
- Assembly bridges contain only the required register/flags/stack adaptation and control transfer. Document inputs, outputs, clobbers, stack cleanup/alignment, continuation and associated build evidence. Keep capture policy/encoding/error logic in C++.
- Never place our objects in game memory or free game allocations with our allocator. Engine string changes go through the verified engine operation.
- Keep DllMain and static/global initialization minimal. No blocking shutdown in DLL detach. Global state is limited to the required bootstrap/hook routing and has explicit lifetime/thread rules.

## 8. Comments, headers and source organization

- Comments explain why, invariants, ownership, ABI evidence or a non-obvious constraint. Do not narrate each assignment or add section comments to disguise a function containing unrelated work.
- Public operations document non-obvious preconditions, ownership, thread requirements and failure semantics. Do not generate a wall of boilerplate documentation for an obvious trivial accessor.
- Name the reason for a workaround and the condition that would permit removal. TODOs describe concrete remaining work; no unexplained TODO or guessed compatibility claim.
- A header is self-contained. Include what it uses; do not rely on another unrelated header importing a type. No `using namespace` directives in headers or implementation files; a narrow using declaration is acceptable locally when it improves clarity.
- Use pragma once for our headers. Keep definitions out of headers unless required for templates, inline constants or small operations. Do not move substantial implementation into headers by default.
- Include groups: corresponding own header; other project headers; third-party headers; standard library; Windows/platform headers. Separate groups by one blank line and sort within groups. Put an unavoidable Windows macro/header ordering requirement in a narrow platform header with a reason.
- Keep Windows macros/types out of portable protocol/domain headers when possible. Explicit ABI headers may necessarily include the SDK. Preserve required upstream/SDK spelling and calling-convention macros.
- Use anonymous namespaces for implementation-private functions/constants. Do not introduce hidden mutable state merely because a variable has internal linkage.

## 9. Formatter, diagnostics and review

Tooling lives beside this guide:

- `.clang-format`: Allman / four spaces / 100 columns / expanded bodies.
- `.clang-tidy`: a focused check set. Configure it against each target's actual compile database. Braces, naming and misleading-indentation diagnostics are errors; function-size/complexity findings are review prompts.

Use LLVM 22.1.3 for reproducible formatting and lint checks, and run formatting only on our own sources. Do not reformat vendor code, game disassembly or copied research references. Clang-format cannot discover the right blank-line groups or enforce architectural cohesion. Tool versions can be updated deliberately after reviewing resulting changes.

Use strong compiler diagnostics for our own targets, initially MSVC /W4 with appropriate conformance switches. Fix meaningful warnings. Suppress an intentional ABI issue locally with an explanation; do not blanket-disable warning categories or apply automatic tidy fixes across hook code without review. Configure third-party warnings separately.

Function-size/complexity thresholds are review prompts. A shorter function full of trivial wrappers is not automatically better. Test failure/cancellation/state transitions and ownership boundaries; do not add tests solely mirroring getters or implementation lines.

Before accepting a generated change, check:

1. Can each file/function's responsibility be stated clearly? Are platform, session and encoder concerns separated?
2. Are all control bodies braced and expanded, with meaningful whitespace grouping?
3. Can the normal path be followed without tracing deeply nested conditions or obscure helpers?
4. Is every resource owner, borrowed lifetime and async ownership transfer explicit?
5. Are error results checked, original platform codes preserved and failures reported at the right boundary?
6. Are thread ownership, lock scopes, cancellation and shutdown behavior understandable?
7. Are unsafe casts/ABI facts localized and supported by evidence?
8. Does the hot path avoid unexpected allocation/blocking beyond documented backpressure?
9. Are we using actual C++20 facilities, with no unnecessary framework or newer-standard dependency?
10. Have formatter/compiler/relevant tests run, and has the diff still received a cohesion/readability review afterward?

## 10. Guidance for code-generation work

Treat the rules above as acceptance criteria, not optional cleanup after a large implementation. Generate one cohesive change at a time. Decide the responsibility, owner, thread contract and failure behavior before writing a component. Read neighboring interfaces before inventing a competing abstraction.

Use short complete functions and meaningful paragraphs of code from the outset. After generation, explicitly review grouping, naming and cohesion; do not assume that successful compilation or formatting means these are good. Remove abandoned alternatives, placeholder scaffolding and speculative fallback paths before presenting a change.

If a rule conflicts with a verified ABI or measured requirement, make the smallest local exception and explain the reason. A required ABI exception does not justify weakening the style of the whole module.

## References

- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines): particularly F.1–F.3 (functions), R.1 (RAII), I.11 (ownership), CP.20/CP.22/CP.43 (locking), E.6 (RAII/error handling), NL.15 (whitespace), NL.16 (member order), and NL.20/NL.21 (statements/declarations). The project's exact naming and brace choices are our conventions.
- [Clang-format options](https://clang.llvm.org/docs/ClangFormatStyleOptions.html).
- [Braces-around-statements check](https://clang.llvm.org/extra/clang-tidy/checks/readability/braces-around-statements.html).
- [Identifier naming check](https://clang.llvm.org/extra/clang-tidy/checks/readability/identifier-naming.html).
- [Function cognitive complexity check](https://clang.llvm.org/extra/clang-tidy/checks/readability/function-cognitive-complexity.html).
