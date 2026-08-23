---
name: reset-generated
description: Delete everything a tracked run created, by reading the run's own event log, plus specification/roadmap/implementation/ - the solution's own output directory, which is cleared whole. No product directory is ever named, so it works unchanged elsewhere. Dry-run first, then apply. Never touches codegen/, the run logs, .claude/, .env, or GitHub issues.
---

# Skill: Reset Generated

Clear the generated output so the next run starts from nothing. This is the *reset* half
of the project's cycle: **generate → observe → reset → regenerate.**

## The mechanism: the log is the manifest

This skill names no output directories. It does not know what `server/` or `firmware/` are, and
it does not need to — **the run recorded what it created**, so the run itself says what to
remove:

| The log says | Used for |
|---|---|
| `issue.commit`, `finding.fixed`, `harden.finding.fixed` → `data.sha` | which commits this run produced |
| `release.tagged` → `data.tag` | which tags to delete — **and** which commit each release was, since those events carry no sha |

The file list then comes from **git**, not from the log: `git show --diff-filter=A` over
those commits. That distinction matters — a commit's recorded `files` includes files it
*modified*, and deleting one of those would remove something that predated the run. The log
decides *which commits to ask about*; git decides *what was added*.

**That is what makes this portable.** Reusing it in another product needs no configuration:
a different codebase produces different commits, and the same query returns its files.

**`specification/roadmap/implementation/` is cleared whole**, and that is not an exception to the rule above.
The rule is about the **product's** directories — `server/`, `firmware/`, `assets/` — which mean
nothing in the next repository. Three directories belong to the *solution* instead, are the
same wherever these skills run, and are named on purpose: `codegen/` and `.claude/`, which
are never deleted, and `specification/roadmap/implementation/`, which is always cleared. Nine skills write
their issues files, GitHub reports, execution reports and code reviews there by name.

It has to be named, because the log cannot reach it: those documents land in `docs:` commits
that no event mentions, so `--diff-filter=A` is never asked about them. Left in place they
make the next run start on top of the last one's paperwork, with `generate-issues` asking
whether to overwrite each file it already finds.

## Usage

```
/reset-generated [--apply]
```

Dry by default. Nothing is deleted without `--apply`, and never without showing the plan and
asking first.

## Instructions

Run the commands below as written. They are the skill — do not substitute your own
reasoning about which files "look generated", and never type a directory name into any of
them.

### Step 0: Refuse when a reset would destroy work

```bash
git status --porcelain            # MUST be empty
RUN=$(cat codegen/runs/current 2>/dev/null); echo "run: $RUN"
jq -r 'select(.type=="run.end" or .type=="run.aborted") | .type' \
   "codegen/runs/$RUN/events.jsonl" | tail -1     # MUST print run.end or run.aborted
```

**Stop and tell the user** if the tree is dirty (uncommitted work would be lost) or the run
has no terminal event (resetting mid-flight orphans it). Proceeding anyway is the user's
call to make, not yours — ask.

### Step 1: Build the list, from the log

```bash
{ jq -r 'select(.type=="issue.commit" or .type=="finding.fixed"
                or .type=="harden.finding.fixed") | .data.sha // empty' \
     codegen/runs/*/events.jsonl
  jq -r 'select(.type=="release.tagged") | .data.tag // empty' codegen/runs/*/events.jsonl \
    | sort -u | while read -r t; do git rev-list -n 1 "$t" 2>/dev/null; done
} | sort -u | while read -r sha; do
      git show --diff-filter=A --name-only --format= "$sha" 2>/dev/null
    done | grep -v '^$' | sort -u \
  | grep -vE '^(codegen|\.claude|\.git|\.venv)(/|$)' \
  | grep -vE '^\.env' \
  | grep -vE '^\.gitignore$|^\.envrc$' \
  > /tmp/reset-files.txt
wc -l < /tmp/reset-files.txt
```

The two `grep -v` lines are the protection, and they are **not optional** — see *What is
never touched*. `.env*` is excluded as a whole, including `.env.example`: losing a template
is harmless, losing an API key may not be recoverable.

Split off anything that looks like **source a person wrote**, which is reported but never
deleted:

```bash
grep -E '^specification/|^[^/]+\.md$|^LICENSE$' /tmp/reset-files.txt \
  | grep -v '^specification/roadmap/implementation/' > /tmp/reset-withheld.txt
grep -vxF -f /tmp/reset-withheld.txt /tmp/reset-files.txt > /tmp/reset-delete.txt
```

Two plain greps rather than one with a negative lookahead: `grep -P` is unavailable on a
stock BSD/macOS `grep`, and this skill is meant to be copied into other repositories. An
empty `reset-withheld.txt` is fine — `grep -vxF -f` on an empty pattern file passes every
line through, which is the safe direction.

### Step 2: Show the plan and get confirmation

```bash
echo "== files this run created ($(wc -l < /tmp/reset-delete.txt)) =="; cat /tmp/reset-delete.txt
echo "== withheld, looks like source =="; cat /tmp/reset-withheld.txt
echo "== tags this run cut =="
jq -r 'select(.type=="release.tagged") | .data.tag' codegen/runs/*/events.jsonl | sort -u
echo "== build residue =="
find . -type d \( -name __pycache__ -o -name '*.egg-info' -o -name .pytest_cache \
     -o -name .mypy_cache -o -name .ruff_cache \) -prune -not -path './codegen/*' -not -path './.venv/*' \
     -not -path './.git/*'
find . -maxdepth 1 -name '*.db'
echo "== specification/roadmap/implementation/ -- cleared whole, see above =="
ls -A specification/roadmap/implementation 2>/dev/null | wc -l
echo "== present but no run claims them -- LEFT ALONE =="
git ls-files | grep -vE '^(codegen|\.claude|spec|\.github)/' \
  | grep -vE '^[^/]*\.md$|^LICENSE$|^\.gitignore$|^\.env' \
  | grep -vxF -f /tmp/reset-files.txt
```

**Show that output and ask before applying.** This is destructive and irreversible short of
git. The last list matters: a file present that no run claims usually means the run made
something without recording it — worth understanding before you delete anything else.

### Step 3: Apply, then commit

Only with `--apply` **and** the user's confirmation:

```bash
while IFS= read -r f; do rm -f -- "$f"; done < /tmp/reset-delete.txt
rm -rf -- specification/roadmap/implementation      # the solution's own output directory; see above
find . -type d \( -name __pycache__ -o -name '*.egg-info' -o -name .pytest_cache \
     -o -name .mypy_cache -o -name .ruff_cache \) -prune -not -path './codegen/*' -not -path './.venv/*' \
     -not -path './.git/*' -exec rm -rf {} +
find . -maxdepth 1 -name '*.db' -delete
jq -r 'select(.type=="release.tagged") | .data.tag' codegen/runs/*/events.jsonl \
  | sort -u | while read -r t; do git tag -d "$t"; done
find . -type d -empty -not -path './.git/*' -not -path './codegen/*' \
     -not -path './.claude/*' -not -path './.venv/*' -delete
git status --short
```

Then commit the deletion. The next `/ship-phase` run now starts from a clean tree.

**Verify before committing** — the guarantees are yours to check now that nothing else does:

```bash
test -f .env && echo "OK .env survived" || echo "FAIL .env was deleted"
test -f .gitignore && echo "OK .gitignore survived"
test -d codegen/runs && echo "OK the logs survived"
test -d .claude/skills && echo "OK the skills survived"
git tag | grep -q . && echo "FAIL tags remain: $(git tag | tr '\n' ' ')" || echo "OK no tags left"
test -d specification/roadmap/implementation && echo "FAIL specification/roadmap/implementation survived" \
  || echo "OK specification/roadmap/implementation cleared"
test -f specification/ARCHITECTURE.md && echo "OK the specs themselves survived"
```

## What is never touched

| | Why |
|---|---|
| **`codegen/`** | the tracker — and `codegen/runs/`, where the logs live. **The logs are the product of the run**; deleting them destroys exactly what the generation was performed to produce. |
| **`.claude/`** | **the skills.** A fix to a skill is *source*, not output. An edited skill was always safe — `--diff-filter=A` lists additions only — but a skill file **created** during a run would otherwise have entered the deletion set. Naming this here does not reintroduce the portability problem: `codegen/` and `.claude/` are the *tooling*, identical in every product, unlike `server/` or `games/`. |
| **`.env*`, `.envrc`, `.gitignore`** | local secrets and the rule that hides them. Today the `--diff-filter=A` query would already spare a gitignored file — but loosen the ignore rule, or `git add -f` once, and a real API key enters the list. A leftover `.env` is an annoyance; a deleted one may be unrecoverable. |
| **GitHub issues** | they carry the issue-id counter. `generate-issues` resolves the next id from `max(GitHub, local) + 1`, so wiping them restarts numbering at 001 and collides with everything already shipped. This skill never calls `gh`. |
| **Anything unclaimed** | reported, never removed. |

## Important Rules

- **Never delete without showing the plan and asking.** Dry run first, always.
- **Run the commands as written.** The protection lives in them. A file list assembled by
  judgement instead — "this looks generated" — is how a `.env` or a spec document gets
  deleted.
- **Release tags must go.** Both orchestrators skip any version whose tag exists, so a reset
  that leaves them makes the next run silently do nothing — the worst outcome, because it
  looks like success. `git tag -d` is local only; if the tags were pushed, deleting them on
  the remote is a separate, explicit decision — ask.
- **Never call `gh`.** Not to close issues, not to delete them.
- **Never edit the log to make a reset tidier.** The log is evidence; if it disagrees with
  the tree, that disagreement is the finding.
- **Source-shaped additions are withheld, not deleted.** Anything a run added that looks
  like source — a `specification/` document outside `roadmap/implementation/`, a root `.md`, `LICENSE` — is
  reported and left in place. A run adding source is unusual enough that a person should
  decide, not a heuristic. Delete them by hand if you are sure.
- **Never name a directory of the PRODUCT.** `server/`, `firmware/`, `assets/` mean nothing in the
  next repository, and typing one here breaks the mechanism. The solution's own three —
  `codegen/`, `.claude/`, `specification/roadmap/implementation/` — are a different thing: they are identical
  wherever these skills run, and the first two are kept while the third is cleared.
