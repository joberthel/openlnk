# Legal Compliance & Clean-Room Architecture Charter (DE / EU)

## 1. Statutory Legal Basis

This project operates strictly within the statutory interoperability framework of German and European Union copyright law:

- **§ 69d Abs. 3 UrhG (Observation, Study, and Testing):** Authorizes the lawful user of a computer program to observe, study, or test the functioning of the program in order to determine the ideas and principles underlying any element of the program while performing authorized acts of loading, displaying, running, transmitting, or storing.
- **§ 69e UrhG (Decompilation for Interoperability):** Permits decompilation exclusively to the extent indispensable to obtain the information necessary to achieve the interoperability of an independently created computer program with other programs, where such information has not previously been readily available.
- **EU Directive 2009/24/EG (Software Directive, Art. 5(3) & Art. 6):** Harmonized European basis for interoperability analysis and clean-room reimplementation.

## 2. Dual-Agent Clean-Room Isolation Model

To prevent any copyright infringement or derivative-work contamination, analysis and implementation are strictly partitioned into two isolated workspaces and agent roles:

| Parameter | Agent A (Dirty / Reverse Engineer) | Agent B (Clean / Implementation Engine) |
| :--- | :--- | :--- |
| **Workspace Path** | `analysis/` | `cleanroom/`, `esp32/` |
| **Permitted Inputs** | Lawfully obtained original binaries (`INTERLNK.EXE`, `INTERSVR.EXE`), dynamic serial traces, Ghidra 12.x analysis | Formal Kaitai Struct specifications (`specs/*.ksy`), clean Markdown protocol specs (`docs/protocol.md`), JSON test vectors (`tests/fixtures/*.json`) |
| **Prohibited Inputs** | Any leaked MS-DOS source code (e.g. `c6ers/interlnk` or similar repositories) | All disassemblies, Ghidra projects, original binaries, raw Agent A prompts/notes |
| **Permitted Outputs** | `specs/interlnk_wire.ksy`, `docs/protocol.md`, `tests/fixtures/*.json`, sanitized Golden Traces (`tests/traces/`) | `cleanroom/` C99 source code, `esp32/` firmware HAL, DOS 16-bit Open Watcom targets |
| **Verification** | Deterministic Python verifiers (`analysis/verifiers/`), Golden Trace validation | C99 unit tests, Kaitai test harness, Differential Oracle emulation |

## 3. Absolute Prohibitions

1. **Zero Use of Leaked Source Code:** Accessing, referencing, copying, or consulting leaked MS-DOS 5.0/6.0/6.22 source repositories (including but not limited to `c6ers/interlnk` or Microsoft MS-DOS source leaks) is strictly forbidden for both Agent A and Agent B.
2. **No Proprietary Binaries in Git:** Original MS-DOS binaries (`INTERLNK.EXE`, `INTERSVR.EXE`, DOS disk images `.vhd`/`.img`) must never be committed to version control. They reside exclusively in gitignored local paths (`analysis/binaries/`).
3. **No Prose Code Leakage Across the Wall:** Information crossing from Agent A to Agent B must be strictly functional, declarative, and machine-verifiable (Kaitai Struct wire formats, state transition specs, and black-box JSON input/output test vectors). No decompiled C pseudo-code, assembly listings, or implementation structure from original binaries may appear in `specs/`, `docs/`, or `cleanroom/`.

## 4. Technical Enforcement Controls

Clean-room separation is enforced automatically via:

- **Pre-Commit Hook (`.githooks/pre-commit`)** and **CI Gate (`tools/ci/check_cleanroom_isolation.py`)**:
  - Blocks any cross-boundary import, `#include`, path reference, or symbol referencing `analysis/`, Ghidra files, or disassembly artifacts from inside `cleanroom/` or `esp32/`.
  - Scans all tracked files for forbidden leaked-source signatures.
  - Prevents a single commit or pull request from simultaneously modifying `analysis/` and `cleanroom/` (ensuring Agent A and Agent B commits remain strictly separated in git history).
  - Enforces `libopenlnk` architectural invariants (zero dynamic memory allocation, zero floating-point arithmetic).

## 5. Immutable Audit Logging

All actions performed by Agent A and Agent B are recorded in dedicated append-only audit logs at the repository root:

- `audit_agent_a.log` — Records every binary import, Ghidra headless run, trace capture session, verifier run, and specification publication by Agent A.
- `audit_agent_b.log` — Records every clean-room implementation session, spec version consumed, test-vector verification, and build run by Agent B.

Each log entry follows an append-only JSON Lines (`.jsonl`-compatible) format with UTC ISO-8601 timestamp, actor, action, inputs consumed, and artifacts produced:

```json
{"ts":"2026-11-26T00:00:00Z","agent":"AGENT_A","action":"IMPORT_BINARY","inputs":["local:INTERLNK.EXE"],"outputs":["specs/interlnk_wire.ksy"],"compliance":"69e_UrhG"}
```
