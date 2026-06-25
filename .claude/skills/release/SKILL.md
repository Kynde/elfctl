---
name: release
description: Cut an elfctl release. Use when the user runs /release with a bump level (patch, minor, or major). Computes the next version from the latest git tag, drags the ELFCTL_VERSION macro in elfctl.c along, pushes master, and creates the v-prefixed GitHub release with an oldest-first changelog.
argument-hint: patch | minor | major
---

# /release — cut an elfctl release

Invoked as `/release <level>`, where `<level>` is exactly `patch`, `minor`, or `major`.
If the argument is missing or anything else, stop and ask which level.

## elfctl release conventions (don't deviate)

- The **git tag is the source of truth** for the release number, and tags/titles are **`v`-prefixed** (`v0.1.0`).
- The next version is computed by bumping the **latest tag**, NOT the source macro — `ELFCTL_VERSION` in `elfctl.c` may already be bumped ahead of the latest tag, so trusting it would skip a number.
- The version lives in **`elfctl.c`** as `#define ELFCTL_VERSION "0.1.0"` — the **bare** number (no `v`), surfaced by `elfctl --version`. It is dragged along to match the tag.
- `gh release create` makes a **lightweight** tag; no binary/artifact is attached (the tool is `make`-built from source).
- `gh release create` creates the tag on the **remote only** — fetch it back afterward so the local tree has it, otherwise `git describe` keeps reporting the *previous* tag plus a commit count.
- Changelog = `git log --reverse` subjects (oldest first) over `<latestTag>..HEAD`, with the `Bump version to ...` bump commit filtered out.

## Steps

**1. Pre-flight.** Stop and report if any fails:
- On `master`: `git rev-parse --abbrev-ref HEAD`
- Clean tree: `git status --porcelain` is empty
- Sync tags from the remote: `git fetch --tags origin` — the latest tag is the source of truth, so a stale local tag list would otherwise compute a version that already exists on the remote.
- Builds clean: `make clean && make` exits 0 with no warnings.

**2. Compute the next version from the latest tag:**
```bash
level="$1"   # patch | minor | major
latest=$(git tag --list 'v*' --sort=-v:refname | head -1)   # e.g. v0.1.0
if [ -z "$latest" ]; then
  # No tags yet (first release). Default the first tag to v0.1.0 regardless
  # of level; confirm with the user before proceeding.
  next="0.1.0"; tag="v0.1.0"
  echo "(no existing tags) -> $tag"
else
  IFS=. read -r MA MI PA <<< "${latest#v}"
  case "$level" in
    major) MA=$((MA+1)); MI=0; PA=0 ;;
    minor) MI=$((MI+1)); PA=0 ;;
    patch) PA=$((PA+1)) ;;
    *) echo "bad level: $level"; exit 1 ;;
  esac
  next="$MA.$MI.$PA"        # bare, for ELFCTL_VERSION
  tag="v$next"             # v-prefixed, for the tag/release
  echo "$latest -> $tag"
fi
```
Sanity-check the computed tag is free (it should be, after the pre-flight fetch). If it already exists, stop — the local list was stale or `$latest` is wrong:
```bash
git rev-parse -q --verify "refs/tags/$tag" >/dev/null && { echo "tag $tag already exists"; exit 1; }
```

**3. Drag the version macro along.** Read the current value:
```bash
cur=$(sed -n 's/.*ELFCTL_VERSION "\(.*\)".*/\1/p' elfctl.c)
```
If `$cur` does NOT already equal `$next`, update the macro in `elfctl.c` to `$next`, rebuild to confirm `elfctl --version` prints `$next`, then commit exactly:
```bash
git commit -am "Bump version to $next"
```
If it already equals `$next` (it may be pre-bumped), skip this commit — don't create an empty one.

**4. Preview & confirm — this step is outward-facing, so pause here.** Show the user `$latest -> $tag` and the changelog preview:
```bash
git log --reverse --pretty='- %s' "$latest"..HEAD | grep -v '^- Bump version to '
```
(For the first release, with no `$latest`, preview the whole history: `git log --reverse --pretty='- %s'`.)
Wait for the user's go-ahead before pushing/releasing.

**5. Push & create the release:**
```bash
git push origin master
notes=$(git log --reverse --pretty='- %s' "$latest"..HEAD | grep -v '^- Bump version to ')
gh release create "$tag" --target master --title "$tag" --notes "$notes"
```

**6. Fetch the new tag back.** `gh release create` made the tag on the remote only, so pull it down so `git describe` resolves to the just-cut tag (not the previous one + commit count):
```bash
git fetch --tags origin
git describe --tags    # should print exactly "$tag"
```

**7. Report** the release URL that `gh` prints.
