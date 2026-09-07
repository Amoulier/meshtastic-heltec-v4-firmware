#!/usr/bin/env bash
# Run the same complete native audit as GitHub; never modify the working source.
set -euo pipefail

main() {
	ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
	cd "$ROOT_DIR"
	BASELINE="$(python3 bin/buildinfo.py json)"
	source_unchanged() {
		[[ "$(python3 bin/buildinfo.py json)" == "$BASELINE" ]] || {
			echo "Source changed during native validation; rerun the audit" >&2
			return 1
		}
	}
	GROUPS_TO_RUN=(shared storage)
	REBUILD=false
	while [[ $# -gt 0 ]]; do
		case "$1" in
		--group)
			[[ $# -ge 2 && $2 =~ ^(shared|storage)$ ]] || {
				echo "Expected --group shared|storage" >&2
				exit 2
			}
			GROUPS_TO_RUN=("$2")
			shift 2
			;;
		--rebuild)
			REBUILD=true
			shift
			;;
		*)
			echo "Usage: $0 [--group shared|storage] [--rebuild]" >&2
			exit 2
			;;
		esac
	done

	docker info >/dev/null
	NETWORK="${HELTEC_DOCKER_NETWORK:-bridge}"
	BUILD_NETWORK="$NETWORK"
	[[ $BUILD_NETWORK != bridge ]] || BUILD_NETWORK=default
	IMAGE_KEY="$(cat "$ROOT_DIR/Dockerfile.test" "$ROOT_DIR/test/host/native-requirements.txt" | sha256sum | cut -c1-20)"
	IMAGE_NAME="heltec-native-test:$IMAGE_KEY"
	if $REBUILD || ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
		tar -C "$ROOT_DIR" -cf - Dockerfile.test test/host/native-requirements.txt |
			docker build --platform linux/amd64 --network "$BUILD_NETWORK" -t "$IMAGE_NAME" -f Dockerfile.test -
	fi

	WORK_BASE="${HELTEC_CI_WORK_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/heltec-ci}"
	mkdir -p "$WORK_BASE"
	WORK_BASE="$(cd "$WORK_BASE" && pwd)"

	for GROUP in "${GROUPS_TO_RUN[@]}"; do
		REPORT_BASE="$ROOT_DIR/.pio/ci/native/$GROUP"
		CACHE="$WORK_BASE/native-cache/$IMAGE_KEY/$GROUP"
		mkdir -p "$REPORT_BASE" "$CACHE/home" "$CACHE/platformio" "$CACHE/build" "$CACHE/libdeps"
		exec {CACHE_LOCK}>"$CACHE/audit.lock"
		flock -n "$CACHE_LOCK" || {
			echo "A native $GROUP audit already holds this cache" >&2
			exit 2
		}
		REPORT="$(mktemp -d "$REPORT_BASE/run-XXXXXXXX")"
		echo "Native $GROUP audit reports: $REPORT"
		WORKSPACE="$(mktemp -d "$WORK_BASE/native-$GROUP-XXXXXXXX")"
		python3 "$ROOT_DIR/bin/ci-snapshot.py" "$ROOT_DIR" "$WORKSPACE" >"$REPORT/snapshot.json"
		cp "$WORKSPACE/.pio/ci-snapshot.json" "$CACHE/ci-snapshot.json"
		printf '%s\n' "$BASELINE" >"$REPORT/baseline.json"
		source_unchanged
		RUN_STATUS=0
		docker run --rm --platform linux/amd64 --network "$NETWORK" \
			--user "$(id -u):$(id -g)" \
			--env HOME=/cache/home --env PLATFORMIO_CORE_DIR=/cache/platformio \
			--env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
			--volume "$WORKSPACE:/workspace" --volume "$CACHE:/cache" \
			--volume "$CACHE:/workspace/.pio" \
			"$IMAGE_NAME" bash -euc '
            mkdir -p audit-results
            python3 --version > audit-results/environment.txt
            g++ --version >> audit-results/environment.txt
            pio --version >> audit-results/environment.txt
            pip freeze >> audit-results/environment.txt
            dpkg-query -W > audit-results/system-packages.txt
            python3 bin/prepare-heltec-native.py
            python3 bin/run-heltec-native-audit.py --group "$1"
        ' native-audit "$GROUP" 2>&1 | tee "$REPORT/run.log" || RUN_STATUS=$?
		docker image inspect "$IMAGE_NAME" >"$REPORT/image.json"
		if [[ -d "$WORKSPACE/audit-results" ]]; then
			cp -a "$WORKSPACE/audit-results/." "$REPORT/"
		fi
		if [[ $RUN_STATUS -eq 0 ]] && ! source_unchanged; then RUN_STATUS=1; fi
		printf '%s\n' "$RUN_STATUS" >"$REPORT/launcher-exit-code.txt"
		if [[ $RUN_STATUS -ne 0 ]]; then
			echo "Native audit failed; source snapshot retained at $WORKSPACE" >&2
			exit "$RUN_STATUS"
		fi
		# mktemp created this directory beneath our resolved cache; never remove an input path.
		[[ $WORKSPACE == "$WORK_BASE"/native-"$GROUP"-* && ! -L $WORKSPACE ]]
		rm -rf -- "$WORKSPACE"
		flock -u "$CACHE_LOCK"
		exec {CACHE_LOCK}>&-
	done
}

main "$@"
