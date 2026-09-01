# OpenAnt scan knobs (gradual ramp)

`openant scan` always: parse → app-context → enhance → Stage 1 analyze → report.

**Opt-in:** `--verify` (Stage 2 attacker simulation).

**On unless you skip:** Docker dynamic tests (`--skip-dynamic-test` to turn off).

`--limit` caps **analyze + enhance** unit count. Parse still walks the whole tree you pointed at. `--llm-reachability` also ignores `--limit` (cost ~ whole tree).

`--limit` is units, not dollars.

---

## Pipeline (what each piece *does*)

| Step | Default | What it does | Cost / time |
|---|---|---|---|
| Parse | always | Walk source, build units + call graph | CPU only |
| `--level` | `reachable` | Which units survive into LLM stages | See table below |
| App context | on (`--no-context` skips) | Classify app type (web / CLI / library / …) so Stage 1 knows intended behavior | 1 LLM pass |
| Enhance | on (`--no-enhance` skips) | Extra call-path / data-flow context for each kept unit | High if `agentic` |
| Stage 1 analyze | always | “Is this unit vulnerable?” | Main LLM spend |
| `--verify` | **off** | Stage 2: role-play attacker; kills theoretical FPs | High (tools + long prompts) |
| Dynamic test | **on** | LLM writes an exploit test, runs it in Docker | Docker + more LLM |
| Report | on (`--no-report` skips) | HTML / markdown / disclosures | Cheap |

Stage 1 = detection. Stage 2 = “can an attacker actually exploit this?” Dynamic test = “did a container reproduce it?”

---

## `--level` (which units reach the LLM)

Default: `reachable`.

| Level | Keeps | Use when |
|---|---|---|
| `reachable` | Units reachable from entry points | Normal scan |
| `all` | Every parsed unit | Library / no clear entry points; expensive |
| `codeql` | Reachable **and** CodeQL-flagged | Need CodeQL CLI + packs; smaller set |
| `exploitable` | After enhance, only units classified exploitable | Cheapest LLM; **weak on C/C++** (enhancer examples are Python/JS-heavy) |

For ESP-IDF / C components: stay on `reachable` (or `all` if you think entry-point detection is dropping the public API). Prefer `--library-mode` over `all` if the problem is “public API dropped as unreachable.”

`--library-mode`: treat exported public API as entry points. Blunt (keeps a lot). Use if `reachable` looks empty on a library/component.

---

## Volume / spend

| Flag | Default | Function |
|---|---|---|
| `--limit N` | `0` = unlimited | Max units for **enhance + analyze**. Best first throttle. |
| `-l c` / `python` / `go` / … | `auto` | Pin language. `auto` = every detected language in that tree. |
| `--workers N` | `8` | Parallel LLM calls. Lower if the company gateway rate-limits. |
| `--backoff N` | `30` | Seconds to wait on 429. |
| `--llm-config espressif` | file `default_llm` | Which provider/model map in `~/.config/openant/config.json`. |

Point `init`/`scan` at a **subdirectory** = that folder is the repo root. Callers outside it are invisible.

---

## Enhance

| Flag | Default | Function |
|---|---|---|
| `--enhance-mode agentic` | default | Tool-using loop (thorough, slower, better context) |
| `--enhance-mode single-shot` | | One LLM call per unit (faster, shallower) |
| `--no-enhance` | | Skip enhance entirely (Stage 1 sees raw units) |

---

## `--verify` (Stage 2)

Off unless you pass `--verify`.

Without it you get Stage 1 findings (more FPs). With it the model tries to exploit from “browser / no server access” and can drop findings that are not actually reachable.

Turn this on **after** Stage 1 looks sane, not on the first smoke test.

---

## Dynamic tests (Docker)

Default: **run**. CLI **exits before the scan** if Docker is missing.

| Flag | Function |
|---|---|
| *(omit)* | Generate + run isolated exploit tests for findings |
| `--skip-dynamic-test` | Skip Docker entirely (LLM-only) |

Tests run in a locked-down container (no privileges, cap-drop, memory/CPU limits, usually no network). Verdicts: `CONFIRMED`, `NOT_REPRODUCED`, `BLOCKED`, `INCONCLUSIVE`, `ERROR`.

Need Docker **and** a finding worth testing. Skip until Stage 1/`--verify` are useful.

---

## Extra (leave off until a full scan is cheap enough)

| Flag | Default | Function |
|---|---|---|
| `--llm-reachability` | off | Extra LLM pass over **the whole tree** to find extra entry points. Cost scales with repo size, **not** `--limit`. |
| `--llm-reachability-max-code-bytes` | `1500` | Bytes of each unit sent to that pass. Raise only if handlers are huge. |
| `--no-context` | off | Skip app-type classification (more FPs). |
| `--no-report` | off | Skip HTML/markdown. |

---

## Smaller than a full tree (git)

Init the **git root**, then:

| Flag | Function |
|---|---|
| `--diff-base origin/main` | Only units overlapping that diff |
| `--pr 123` | Same vs a GitHub PR base (`gh` required) |
| `--staged` | Staged index vs HEAD |
| `--incremental` | Vs last successful scan of this project |
| `--diff-scope` | `changed_functions` (default), `changed_files`, `callers` |
| `--full` | Force full scan (conflicts with incremental/diff flags) |

Diff needs a real git repo. A bare component folder with no `.git` cannot use these.

---

## Suggested ladder

Assume project already inited, `--llm-config espressif`, C component.

**1. Smoke**  
`--skip-dynamic-test --limit 5`  
Prove gateway + parse + a few Stage 1 calls.

**2. More units, still no Docker / Stage 2**  
`--skip-dynamic-test --limit 25` then `--limit 100`, then drop `--limit`.  
Keep `-l c`. Optional: `--enhance-mode single-shot` if enhance dominates time.

**3. Better detection quality**  
Remove `--enhance-mode single-shot` (back to `agentic`). Still `--skip-dynamic-test`. Still no `--verify`.

**4. Kill theoretical FPs**  
Add `--verify`. Still `--skip-dynamic-test` until Docker is ready.

**5. Reproduction**  
Drop `--skip-dynamic-test` (Docker running). This is the full product path: Stage 1 → Stage 2 → container exploit.

**6. Wider code**  
Init a larger tree (or the whole repo). Same flags as step 5.  
Only then consider `--llm-reachability` or `--level all` / `--library-mode`.

Example commands:

```bash
# 1
openant scan --llm-config espressif --skip-dynamic-test --limit 5

# 2
openant scan --llm-config espressif --skip-dynamic-test --limit 25

# 3
openant scan --llm-config espressif --skip-dynamic-test

# 4
openant scan --llm-config espressif --verify --skip-dynamic-test

# 5
openant scan --llm-config espressif --verify

# 6 (full tree + extra entry-point hunt — expensive)
openant scan /path/to/full-repo -l c --llm-config espressif --verify --llm-reachability
```

Timeout: `OPENANT_INVOKE_TIMEOUT` (default `30m`) covers the whole `scan`. Raise (`2h`) on bigger trees.

---

## Exit codes (`openant scan`)

| Code | Meaning |
|---|---|
| 0 | Clean (no vulns) |
| 1 | Findings (success) |
| 2 | Scan itself failed |

Do not treat `1` as a CI infrastructure failure.
