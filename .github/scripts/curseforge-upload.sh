#!/usr/bin/env bash
#
# Attach files to an EXISTING CurseForge file using parentFileID, so an updated
# component jar lands as an additional file on the current library entry instead of
# creating (and re-uploading) a whole new entry.
#
# CurseForge has no public API to archive the superseded files; archive those on
# the website (Files -> ... -> Archive) after the run.
#
# Environment:
#   CURSEFORGE_TOKEN       required; CurseForge API token
#   CURSEFORGE_PROJECT_ID  required; numeric CurseForge project id
#
# Usage: curseforge-upload.sh <parentFileId> <file> [<file>...]
#
set -euo pipefail

: "${CURSEFORGE_TOKEN:?CURSEFORGE_TOKEN is required}"
: "${CURSEFORGE_PROJECT_ID:?CURSEFORGE_PROJECT_ID is required}"

parent="${1:?parent file id required}"
shift

if [ "$#" -eq 0 ]; then
  echo "curseforge-upload: nothing to upload"
  exit 0
fi

project_id="${CURSEFORGE_PROJECT_ID}"
upload_url="https://minecraft.curseforge.com/api/projects/${project_id}/upload-file"
# The changelog: the last few commits plus a link to the full release notes.
changelog="$(cat dist/changelog.md 2>/dev/null || echo '')"

case "$parent" in
  ''|*[!0-9]*)
    echo "curseforge-upload: parent file id '${parent}' is not numeric." >&2
    echo "Set cf.<library>.file.<mc> in release-state.properties (the library release records it)." >&2
    exit 1
    ;;
esac

for file in "$@"; do
  name="$(basename "$file")"
  echo "curseforge-upload: attaching ${name} under file ${parent}"
  metadata="$(jq -nc \
    --arg changelog "$changelog" \
    --arg displayName "$name" \
    --argjson parentFileID "$parent" \
    '{changelog: $changelog, changelogType: "markdown", displayName: $displayName,
      parentFileID: $parentFileID, releaseType: "release"}')"

  ok=false
  for attempt in 1 2 3 4 5; do
    if curl -fsS -X POST "$upload_url" \
        -H "X-Api-Token: ${CURSEFORGE_TOKEN}" \
        -F "metadata=${metadata};type=application/json" \
        -F "file=@${file}"; then
      echo "curseforge-upload: ${name} uploaded"
      ok=true
      break
    fi
    echo "curseforge-upload: attempt ${attempt} failed for ${name}" >&2
    sleep 15
  done

  if [ "$ok" != true ]; then
    echo "curseforge-upload: giving up on ${name}" >&2
    exit 1
  fi
done
