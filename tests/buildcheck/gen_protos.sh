#!/bin/sh
# Generate the C++/gRPC sources that mod_google_transcribe's google_glue.cpp
# includes (google/cloud/speech/v1p1beta1/cloud_speech.grpc.pb.h).
#
# Upstream's build expects these under libs/googleapis/gens and does not vendor
# them, so a machine that can build FreeSWITCH still cannot necessarily compile
# this module without generating them first. This script does the generating,
# including the transitive googleapis imports: the produced
# cloud_speech.pb.h includes google/api/annotations.pb.h and friends, so
# compiling the speech proto alone leaves the build short of headers.
#
# Requires: protoc + grpc_cpp_plugin (Debian: protobuf-compiler
# protobuf-compiler-grpc libprotobuf-dev libgrpc++-dev) and git.
#
# usage: gen_protos.sh <out-dir> [<googleapis-checkout-dir>]
set -eu

# Pinned deliberately: tracking googleapis HEAD would make a clean compile
# today and a broken one tomorrow with no change in this repo to blame. Bump
# this SHA on purpose, alongside a re-run of the compile check.
GOOGLEAPIS_SHA=9c085b2eb8a4c9996418d4268ade3fb9c708021d
GOOGLEAPIS_URL=https://github.com/googleapis/googleapis

OUT=${1:?usage: gen_protos.sh <out-dir> [<googleapis-checkout-dir>]}
SRC=${2:-}
CLEANUP=""

if [ -z "$SRC" ]; then
  SRC="$(mktemp -d)/googleapis"
  CLEANUP="$SRC"
  git clone -q --depth 1 "$GOOGLEAPIS_URL" "$SRC"
  git -C "$SRC" checkout -q "$GOOGLEAPIS_SHA"
fi

PLUGIN="$(command -v grpc_cpp_plugin)"
SEED=google/cloud/speech/v1p1beta1/cloud_speech.proto
mkdir -p "$OUT"

# transitive closure of the imports that live in googleapis; well-known types
# (google/protobuf/*.proto) are skipped because their .pb.h ship with
# libprotobuf-dev under /usr/include
cd "$SRC"
all="$SEED"
prev=""
while [ "$all" != "$prev" ]; do
  prev="$all"
  for f in $all; do
    for imp in $(sed -n -E 's/^import +(public )?"([^"]+)".*/\2/p' "$f"); do
      [ -f "$imp" ] || continue
      case " $all " in *" $imp "*) ;; *) all="$all $imp";; esac
    done
  done
done

protoc --proto_path=. --proto_path=/usr/include \
  --cpp_out="$OUT" --grpc_out="$OUT" \
  --plugin=protoc-gen-grpc="$PLUGIN" \
  $all

for h in cloud_speech.pb.h cloud_speech.grpc.pb.h; do
  test -f "$OUT/google/cloud/speech/v1p1beta1/$h" || {
    echo "MISSING $h after protoc" >&2; exit 1; }
done

[ -z "$CLEANUP" ] || rm -rf "$CLEANUP"
echo "generated $(echo $all | wc -w) protos into $OUT (googleapis @ $GOOGLEAPIS_SHA)"
