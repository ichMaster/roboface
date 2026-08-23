---
name: release-version
description: Bump project version, update all version files, add RELEASE.txt entry, commit, tag, and push.
---

# Skill: Release Version

Bump the project version, update all version references, write release notes, commit,
tag, and push.

## Usage

```
/release-version <version> [changelog line 1; changelog line 2; ...]
```

Examples:
- `/release-version 0.1.0` -- bump to 0.1.0, prompt for changelog
- `/release-version 1.2.0 streaming TTS playback; VAD endpointer` -- bump with provided changelog items

If no changelog items are provided, analyze uncommitted or recent commits since the last
tag to auto-generate the changelog.

Version notation `A.B.C` (the Lumi standard): `A` = roadmap version (v0→0 … v6→6), `B` = phase
within that version (`v1.2`→B=2), `C` = a post-release fix on that phase. So roadmap phase `vA.B`
→ release `A.B.0`, tagged `vA.B.0`; a fix after it bumps `C` (e.g. v1.2 → `1.2.0`, a follow-up fix
→ `1.2.1`). No zero padding. Releases are cut per phase. **Never change the version without
explicit user confirmation.**

## Instructions

### Step 0: Parse arguments

1. Extract the target version from the first argument (e.g., `1.2.0`)
2. Remaining arguments (separated by `;`) become changelog bullet points
3. Validate version format matches `A.B.C` (no zero padding) and that `A.B` is a real ROADMAP phase

### Step 1: Verify prerequisites

1. Confirm we are on the expected branch (the current working dev branch)
2. Confirm working tree is clean (`git status`) -- if dirty, ask the user whether to include uncommitted changes
3. Find the current version: check `VERSION`, `RELEASE.txt`, or the latest git tag
4. Verify the new version is greater than the current version

### Step 2: Generate changelog (if not provided)

If no changelog items were given as arguments:

1. Find the most recent version tag: `git describe --tags --abbrev=0`
2. Collect commits since that tag: `git log --oneline <tag>..HEAD`
3. Summarize the changes into concise bullet points (group related commits; reference the roadmap phase `vA.B` where relevant)
4. Show the generated changelog to the user and ask for confirmation

### Step 3: Update version files

1. **`VERSION`** (create if it doesn't exist): the bare version string, e.g. `1.2.0`
2. **`README.md`** (if present): update version reference
3. **FastAPI app version** in `server/roboface_server/main.py` (if present): update the `version=` string on the `FastAPI(...)` app
4. **`RELEASE.txt`** (create if it doesn't exist): prepend a new version block at the top (after any header):

   ```
   Version <version> (YYYY-MM-DD)
   ---------------------------
   - <changelog item 1>
   - <changelog item 2>
   ```

   Use today's date. Keep the existing entries below unchanged.

### Step 4: Commit

Stage only the version-related files — and only the ones that **exist**. Early releases run before
`server/roboface_server/main.py` or `README.md` are generated, and `git add` is **fatal** on a pathspec that matches
nothing (`fatal: pathspec '…' did not match any files`, exit 128), which would abort the release with
the version files already rewritten:

```bash
for f in VERSION README.md RELEASE.txt server/roboface_server/main.py; do
  if [ -e "$f" ]; then git add "$f"; fi
done
```

(Use the `if` form, not `[ -e "$f" ] && git add "$f"` — the latter leaves the loop's exit status at 1
whenever the *last* file is absent, which is exactly the common case here.)

```bash
git commit -m "$(cat <<'EOF'
Release v<version>

<1-2 sentence summary of what this release includes>

Co-Authored-By: <the running model's trailer> <noreply@anthropic.com>
EOF
)"
```

### Step 5: Tag

```bash
git tag -a v<version> -m "<one-line summary of the release>"
```

### Step 6: Push

Push the branch, then **only the tag just created** — never `--tags` or `--follow-tags`:

```bash
git push
git push origin "v<version>"
```

> `git push --tags` pushes *every* local tag, and `--follow-tags` pushes every annotated tag reachable
> from the pushed commits — including any tag a later re-generation run left behind. Push the one tag
> by name so a release publishes exactly what it cut.

### Step 6.5: Emit tracking events

`--emitter skill:release-version --scope phase=..,version=..`: after the tag → `release.tagged`
(`tag`); after the push → `release.pushed` (`tag`, `remote`).

### Step 7: Report

```
Released v<version>
  Branch: <branch>
  Commit: <short hash>
  Tag:    v<version>
  Files updated:
    - VERSION
    - README.md
    - RELEASE.txt
    - server/roboface_server/main.py
```

## Important Rules

- **Never downgrade.** Refuse if the target version is less than or equal to the current version.
- **Clean tree first.** If there are uncommitted changes, ask the user before proceeding.
- **Annotated tags only.** Always use `git tag -a`, never lightweight tags.
- **Don't modify source files.** This skill only touches version metadata (VERSION, README.md, RELEASE.txt, and the FastAPI app version string), never server/firmware/assets logic.
- **Confirm changelog.** If auto-generating changelog from commits, show it to the user before committing.
- **Plain-text release notes.** Keep `RELEASE.txt` plain text.
