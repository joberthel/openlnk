# Agent A Workspace (`analysis/`) — Dirty / Reverse-Engineering Room

> **ISOLATION WARNING:** Agent B (`cleanroom/`, `esp32/`) MUST NEVER read, include, import, or inspect any file inside this directory.

## Legal Scope (§ 69d Abs. 3 & § 69e UrhG)

- Place lawfully obtained MS-DOS 5.00/6.00/6.22 `INTERLNK.EXE` and `INTERSVR.EXE` binaries in `analysis/binaries/` (gitignored).
- **STRICTLY PROHIBITED:** Consulting, storing, or referencing leaked MS-DOS source code repositories (e.g., `c6ers/interlnk`).
- All analysis sessions must append an entry to `audit_agent_a.log`.

## Static Analysis Setup (Ghidra 12.x + MCP)

1. Verify environment and binary placement:
   ```bash
   python3 analysis/setup_ghidra.py --check
   ```
2. Import `INTERLNK.EXE` and `INTERSVR.EXE` as `x86:LE:16:Real Mode` with `DOS_DeviceHeader` and COM1 UART annotations:
   ```bash
   GHIDRA_HOME=/path/to/ghidra_12.x python3 analysis/setup_ghidra.py
   ```
3. Start the MCP server (via `.vscode/mcp.json` or standalone):
   ```bash
   ./analysis/start_ghidra_mcp.sh http
   ```

## Anti-Hallucination Verification Workflow

Never infer checksums or frame offsets manually in prose. Validate every hypothesis deterministically against captured Golden Traces:

```bash
python3 analysis/verifiers/verify_crc.py --payload-hex 313233343536373839 --expected 0x29B1
```

## Formal Handoff Outputs (Allowed Cross-Wall Artifacts)

Agent A may publish **only** declarative, clean-room-safe outputs:
- `specs/interlnk_wire.ksy`
- `docs/protocol.md`
- `tests/fixtures/*.json`
- Sanitized Golden Traces in `tests/traces/`
