#!/usr/bin/env bash
#
# Подготовка релиза форка на базе upstream-тега.
#
# Использование:
#   ./bin/prepare-fork-release.sh v2.7.20.6658ec2 1
#
# Это создаст тег v2.7.20.6658ec2-svk.1 на текущей ветке develop
# после мержа upstream-тега. Без аргументов покажет доступные теги.
#
set -euo pipefail

echo "==> Fetching upstream tags..."
git fetch upstream --tags

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <upstream-tag> <svk-build-number>"
    echo ""
    echo "Available upstream tags (latest 10):"
    git tag -l 'v2.*' --sort=-version:refname | head -10 | sed 's/^/  /'
    echo ""
    echo "Example: $0 $(git tag -l 'v2.*' --sort=-version:refname | head -1) 1"
    exit 1
fi

UPSTREAM_TAG="$1"
SVK_BUILD="$2"
FORK_TAG="${UPSTREAM_TAG}-svk.${SVK_BUILD}"

# Проверяем что upstream-тег существует
if ! git rev-parse "$UPSTREAM_TAG" >/dev/null 2>&1; then
    echo "ERROR: Upstream tag '$UPSTREAM_TAG' not found"
    echo ""
    echo "Available upstream tags (latest 10):"
    git tag -l 'v2.*' --sort=-version:refname | head -10 | sed 's/^/  /'
    exit 1
fi

# Проверяем что мы на develop
CURRENT_BRANCH=$(git branch --show-current)
if [[ "$CURRENT_BRANCH" != "develop" ]]; then
    echo "ERROR: Switch to 'develop' branch first (currently on '$CURRENT_BRANCH')"
    exit 1
fi

# Проверяем что тег ещё не существует
if git rev-parse "$FORK_TAG" >/dev/null 2>&1; then
    echo "ERROR: Tag '$FORK_TAG' already exists"
    exit 1
fi

echo "==> Merging $UPSTREAM_TAG into develop..."
if ! git merge "$UPSTREAM_TAG" -m "Merge upstream $UPSTREAM_TAG for fork release $FORK_TAG"; then
    echo ""
    echo "ERROR: Merge conflicts detected. Resolve them manually, then run:"
    echo "  git commit"
    echo "  git tag $FORK_TAG"
    echo "  git push origin develop --tags"
    exit 1
fi

echo "==> Creating tag $FORK_TAG..."
git tag "$FORK_TAG"

echo "==> Pushing develop and tag to origin..."
git push origin develop
git push origin "$FORK_TAG"

echo ""
echo "Done! GitHub Actions will build and create the release."
echo "Track progress: https://github.com/skrashevich/meshtastic-firmware/actions"
