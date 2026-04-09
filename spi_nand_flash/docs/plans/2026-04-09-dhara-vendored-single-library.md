# Dhara vendored in-tree (remove submodule) — implementation plan

> **For agentic workers:** Use `superpowers:executing-plans` (or equivalent) to implement task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `dhara/dhara` git submodule with ordinary git-tracked files under the existing `dhara/` ESP-IDF component, so Espressif can patch the FTL in-repo without a second component or Kconfig switch, and ship behavior changes only through explicit `espressif/dhara` / `espressif/spi_nand_flash` version bumps.

**Architecture:** Keep a single component name (`dhara`), the same CMake layout (`INCLUDE_DIRS dhara`, `SRC_DIRS dhara/dhara`), and the same include paths (`#include "dhara/map.h"`). The only structural change is that `dhara/dhara/` is no longer a submodule pointer but a full copy of the upstream tree (at a recorded baseline commit). Future upstream merges are manual (cherry-pick, copy, or re-snapshot), documented next to the sources.

**Tech Stack:** Git, ESP-IDF component manager (`idf_component.yml`), CMake (`idf_component_register`), C (Dhara), optional SBOM YAML.

---

## Executive summary (for reviewers)

Today, upstream Dhara lives under `dhara/dhara` as a submodule of `https://github.com/dlbeer/dhara.git`. This plan removes that submodule and commits the same files as normal repository content. **No second library, no CMake/Kconfig fork selector.** Behavioral or FTL changes are communicated through **semver and changelogs** for `dhara` and/or `spi_nand_flash`, not through IDF version defaults or dual linkage.

This is the lowest-complexity option if the project does **not** need to publish a “pristine upstream-only” Dhara tree alongside a fork: the published `espressif/dhara` component **becomes** the Espressif-maintained source tree, still derived from dlbeer/dhara but no longer a pointer to it.

---

## Rationale vs dual-component / Kconfig approach

| Topic | Vendored single `dhara` (this plan) | Two components (`dhara` + `dhara_esp`) |
|--------|--------------------------------------|----------------------------------------|
| Linker / dependency graph | One Dhara always | Risk of two Dhara if any dep still pulls `espressif/dhara` |
| Operational complexity | Submodule removal + provenance docs | Extra component, Kconfig, manifest entries, CI matrix |
| When behavior changes | Explicit component/app version bump | Can add Kconfig/IDF-default coupling if not careful |
| Upstream sync | Manual; must be scheduled | Submodule path stays “easy” for vanilla tree only |

---

## Repository layout (after)

```
idf-extra-components/
├── .gitmodules                         ← dhara submodule entry removed
├── dhara/
│   ├── CMakeLists.txt                  ← unchanged (verify)
│   ├── idf_component.yml               ← bump version / description when behavior changes
│   ├── sbom_dhara.yml                  ← update when baseline or modifications change
│   ├── README.md                       ← vendoring + baseline commit documented
│   ├── VENDORED_UPSTREAM.md            ← create: baseline SHA, how to refresh
│   └── dhara/                          ← ordinary tree (NOT a submodule)
│       ├── LICENSE                     ← keep upstream license file(s)
│       ├── dhara/                      ← library sources (map.c, journal.c, …)
│       ├── ecc/, tests/, …            ← optional but recommended: full tree for easier diffs
│       └── …
└── spi_nand_flash/
    ├── idf_component.yml               ← still depends on espressif/dhara (no new component)
    └── docs/plans/                     ← this file
```

**Important:** `dhara/CMakeLists.txt` today registers `INCLUDE_DIRS dhara` and `SRC_DIRS "dhara/dhara"`. That must remain valid: the vendored tree must preserve path `dhara/dhara/*.c` and `dhara/dhara/*.h` for the library API used by `spi_nand_flash`.

---

## Caveats and what to watch for

### Policy and product

- **Single source of truth:** Any project that depended on `espressif/dhara` being “vanilla upstream at commit X” must accept that the registry component is now **Espressif-vendored**. Document that in `dhara/README.md` and release notes when this lands.
- **Semver is the behavior gate:** FTL, journal, GC, or capacity logic changes should drive **`dhara` `idf_component.yml` version** and/or **`spi_nand_flash` version**, with **CHANGELOG** entries calling out flash / migration impact where relevant.
- **No silent IDF coupling:** Do not tie default Dhara behavior to `IDF_INIT_VERSION`; behavior changes ride on **component versions** the app pins.

### Engineering

- **Initial snapshot must match current submodule commit** (byte-for-byte for shipped sources) so the first merge is a no-op for behavior. Record that commit in `VENDORED_UPSTREAM.md`. Current SBOM reference (verify before doing the migration): `sbom_dhara.yml` uses hash `1b166e41b74b4a62ee6001ba5fab7a8805e80ea2`.
- **After local patches:** Updating `sbom_dhara.yml` is mandatory for compliance tooling. Options: keep `url` pointing at upstream and set `version`/`hash` to baseline + describe modifications in README, or follow org policy for forked packages.
- **Submodule removal hygiene:** Clones must not keep stale `.git/modules/dhara/dhara`. The plan includes cleanup steps; reviewers should verify `git submodule status` no longer lists `dhara/dhara`.
- **What to vendor:** Minimum is what ESP-IDF builds (`dhara/dhara/` library sources). **Recommended:** vendor the **entire** upstream repo contents (tests, ecc, tools, etc.) so future `git diff` / `patch` against upstream is tractable, even if IDF does not compile tests.
- **Duplicate `dhara_glue.c` / CMake drift:** If your branch already changed `spi_nand_flash/CMakeLists.txt` (e.g. WL backends), reconcile separately; this plan does not require a fork-specific CMake split.

### CI and consumers

- **`.github/workflows/upload_component.yml`:** Still uploads `dhara` as today; **no new component name**. Confirm dry-run still passes after the tree change (large file add is fine).
- **`checkout` with `submodules: recursive`:** Remains valid; one fewer submodule.
- **Apps using `espressif/dhara` from registry:** They get the vendored sources inside the tarball the registry stores—**no workflow change** for them unless you bump semver and they upgrade.

### Legal / provenance

- Keep upstream **`LICENSE`** (and `LICENSE` at component root if you duplicate for clarity—follow repo convention).
- In `VENDORED_UPSTREAM.md`, state: upstream URL, **exact commit SHA** of the snapshot, and that subsequent commits may contain Espressif modifications.

---

## Task 1: Record baseline and scope

**Files:**

- Read-only: `dhara/dhara/.git` (submodule HEAD) or `git -C dhara/dhara rev-parse HEAD`
- Create: `dhara/VENDORED_UPSTREAM.md`

- [ ] **Step 1: Record current submodule commit**

Run (from repo root, with submodule initialized):

```bash
git -C dhara/dhara rev-parse HEAD
```

Expected: a full 40-character SHA. **Save this** as `UPSTREAM_BASELINE_SHA` in the task notes. It must match what `dhara/sbom_dhara.yml` claims unless SBOM is already wrong (if wrong, fix SBOM in the same PR as vendoring).

- [ ] **Step 2: Create `dhara/VENDORED_UPSTREAM.md`**

Create a short file (adjust SHA and date):

```markdown
# Dhara upstream baseline

This directory is vendored from **https://github.com/dlbeer/dhara**.

- **Baseline commit:** `<PASTE_SHA_FROM_STEP_1>`
- **Vendored on:** `<YYYY-MM-DD>`
- **Policy:** Espressif may apply patches here. For upstream bugfixes/features, prefer
  cherry-picking or re-baselining to a newer upstream commit, then update this file and
  `sbom_dhara.yml`.

## Refresh procedure (maintainers)

1. Compare this tree to upstream: `git fetch` in a separate clone of dlbeer/dhara, diff against `<SHA>`.
2. Merge or cherry-pick desired commits.
3. Update **Baseline commit** above and `sbom_dhara.yml` (`version` / `hash` per org rules).
4. Bump `dhara/idf_component.yml` version and document user-visible FTL changes in changelogs.
```

- [ ] **Step 3: Commit**

```bash
git add dhara/VENDORED_UPSTREAM.md
git commit -m "docs(dhara): record vendored upstream baseline (pre-migration)"
```

(Alternatively, fold this file into the same commit as the vendored tree if the team prefers a single commit—then skip this early commit.)

---

## Task 2: Remove submodule and add vendored tree

**Files:**

- Modify: `.gitmodules` (remove `dhara/dhara` entry)
- Modify: `dhara/dhara/` (from gitlink to full tree)
- Optional cleanup: `.git/modules/dhara/dhara/` on developers’ machines

- [ ] **Step 1: Export a clean tree (backup)**

With submodule checked out:

```bash
rsync -a --exclude='.git' dhara/dhara/ /tmp/dhara-vendor-export/
```

- [ ] **Step 2: Deinit and remove submodule from git**

From repo root:

```bash
git submodule deinit -f dhara/dhara
git rm -f dhara/dhara
```

Remove the submodule section from `.gitmodules` (the block):

```ini
[submodule "dhara/dhara"]
	path = dhara/dhara
	url = https://github.com/dlbeer/dhara.git
```

Then:

```bash
git add .gitmodules
```

- [ ] **Step 3: Restore files as regular content**

```bash
mkdir -p dhara/dhara
rsync -a /tmp/dhara-vendor-export/ dhara/dhara/
```

Confirm there is **no** `dhara/dhara/.git` directory (nested repo must not reappear).

- [ ] **Step 4: Add and commit**

```bash
git add dhara/dhara .gitmodules
git commit -m "refactor(dhara): vendor dhara sources, remove git submodule"
```

- [ ] **Step 5: Local submodule cleanup (each developer / CI image)**

If `git submodule update` was used heavily, maintainers may need:

```bash
rm -rf .git/modules/dhara/dhara
```

(Only if the directory still exists after removal.)

---

## Task 3: Documentation updates

**Files:**

- Modify: `dhara/README.md`
- Modify: `spi_nand_flash/README.md` (optional wording)
- Modify: `spi_nand_flash/CHANGELOG.md` and/or root changelog policy if this repo uses one for `dhara`

- [ ] **Step 1: Update `dhara/README.md`**

Replace generic “wrapper around” wording with vendoring facts:

- State that sources under `dhara/dhara/` are **vendored** from dlbeer/dhara.
- Point to `VENDORED_UPSTREAM.md` for the baseline SHA.
- Link upstream for background reading.

- [ ] **Step 2: Optional `spi_nand_flash/README.md`**

Clarify that the dependency `espressif/dhara` is fulfilled by vendored sources maintained in this repository (no user-facing submodule steps).

- [ ] **Step 3: Changelog entry**

Add an entry under `dhara` and/or `spi_nand_flash` describing:

- Submodule removed; sources vendored.
- No intentional FTL behavior change in the migration commit (if true).
- Where to find baseline SHA.

---

## Task 4: SBOM and component version

**Files:**

- Modify: `dhara/sbom_dhara.yml`
- Modify: `dhara/idf_component.yml` (version field)

- [ ] **Step 1: Align `sbom_dhara.yml`**

If the vendored snapshot equals the previous submodule commit, fields may stay unchanged except a team policy note. If SBOM must reflect “Espressif modified package,” update per internal compliance (supplier string, version, hash rules).

- [ ] **Step 2: Bump `dhara/idf_component.yml` version**

At minimum, bump **patch** (e.g. `0.1.0` → `0.1.1`) for “packaging change only.” If your org treats submodule→vendor as **minor**, use that instead—**be consistent** with the component registry.

---

## Task 5: CI and automation

**Files:**

- Review: `.github/workflows/upload_component.yml`
- Grep: other workflows referencing `dhara` or submodules

- [ ] **Step 1: Confirm upload list**

`.github/workflows/upload_component.yml` already includes `dhara` under `components:`. **No change required** unless a workflow assumed `dhara/dhara` is a submodule for a custom step (grep the repo).

```bash
rg -n "dhara/dhara|submodule.*dhara" .github --glob "*.{yml,yaml,sh,md}"
```

- [ ] **Step 2: PR checkout**

No change needed to `actions/checkout@v4` with `submodules: recursive`; it continues to work.

---

## Task 6: Verification

- [ ] **Step 1: Clean clone test**

```bash
git clone <repo-url> /tmp/idf-ec-test && cd /tmp/idf-ec-test
git submodule update --init --recursive
# dhara/dhara should populate without separate submodule clone
test -f dhara/dhara/dhara/map.c && echo OK
```

- [ ] **Step 2: Build a consumer**

From an app or test that uses `spi_nand_flash` or `dhara` with `override_path` (e.g. `spi_nand_flash/test_app` or documented example):

```bash
idf.py set-target esp32   # or project default
idf.py build
```

Expected: success; link line includes objects from `dhara/dhara/dhara/*.c` (paths may vary slightly in build logs).

- [ ] **Step 3: Component manager dry-run**

If CI runs `upload-components-ci-action` with dry-run on PRs, confirm the PR passes. If not, run the same action locally per Espressif docs.

---

## Team review checklist

- [ ] Agree that **published `espressif/dhara` may diverge** from dlbeer/dhara at HEAD.
- [ ] Agree on **semver rules** when FTL logic changes (minor vs major).
- [ ] Confirm **baseline SHA** in `VENDORED_UPSTREAM.md` matches the pre-migration submodule commit.
- [ ] **SBOM** updated or explicitly waived with compliance sign-off.
- [ ] **No nested `.git`** under `dhara/dhara`.
- [ ] **CHANGELOG** and **README** updated.
- [ ] **CI green** including component upload dry-run.

---

## Self-review (plan author)

| Requirement | Task coverage |
|-------------|----------------|
| Remove submodule | Task 2 |
| Single library, no dual CMake | Architecture + no Task for second component |
| CI / `.gitmodules` | Tasks 2, 5 |
| Caveats for team | Sections above |
| SBOM / versioning | Task 4 |
| Verification | Task 6 |

---

## Execution handoff

**Plan complete** and saved to `spi_nand_flash/docs/plans/2026-04-09-dhara-vendored-single-library.md`.

**Option A — Subagent-driven:** One task per PR or per agent run, with review between tasks.  
**Option B — Single PR:** Tasks 1–6 in one branch (common for mechanical migrations).

Which approach does the team want?
