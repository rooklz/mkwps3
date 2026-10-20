#!/bin/sh
# translate.sh -- translate the game into the workspace: functions, data
# initialisers and the build shards Makefile.ps3 compiles.
#
# GEN (tools/local.env) is the workspace. It needs:
#   projects/mkwii/   a copy of upstream/wiicompiled/projects/mkwii
#   Assets/           main.dol and StaticR.rel from your own disc (recomp.yml
#                     checks their hashes)
#   runtime ->        a symlink to this repository's runtime/
# The translator is built once with
#   dotnet build upstream/wiicompiled/translator/src/Translator.Cli/Translator.Cli.csproj -c Release
set -e
. "$(dirname "$0")/local.env" 2>/dev/null || true
REPO=$(cd "$(dirname "$0")/.." && pwd)
GEN=${GEN:?set GEN in tools/local.env}
DN=${DOTNET:-dotnet}
TR=$REPO/upstream/wiicompiled/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll
P=projects/mkwii/recomp.yml
cd "$GEN"
$DN "$TR" translate-recursive 0x800060A4 --project $P --outdir generated/functions \
    --output-metadata build/base/translation_metadata.json --threads "${THREADS:-8}" > translate.log 2>&1
echo "translate done: $(ls generated/functions | wc -l) functions"
$DN "$TR" generate-data-init --project $P > datainit.log 2>&1
echo "data init done"
$DN "$TR" emit-build-shards --project $P --base-metadata build/base/translation_metadata.json \
    --base-functions-dir generated/functions --native-source-dir runtime/src --out build/shards > shards.log 2>&1
echo "shards done: $(ls build/shards/base_common/*.cpp | wc -l)"
