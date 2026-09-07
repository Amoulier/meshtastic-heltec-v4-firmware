#!/usr/bin/env bash
main() {
	set -euo pipefail
	root=$(cd "$(dirname "$0")/.." && pwd)
	source "$root/bin/heltec-ci.env"
	mode=${1:-all}
	case "$mode" in
	check | build-standard | build-solar-router | native | all) ;;
	*)
		echo "Usage: $0 [check|build-standard|build-solar-router|native|all]" >&2
		exit 2
		;;
	esac
	if [[ ${HELTEC_DEVCONTAINER:-0} == 1 ]]; then
		echo 'Run Heltec CI tasks from the host; use Heltec Devcontainer tasks inside this container.' >&2
		exit 2
	fi
	if [[ -z ${DOCKER_CONTEXT-} && -z ${DOCKER_HOST-} ]] && docker context inspect heltec-ci >/dev/null 2>&1; then
		export DOCKER_CONTEXT=heltec-ci
		export HELTEC_DOCKER_NETWORK=host
	fi
	docker info >/dev/null
	if [[ $mode == native ]]; then exec bash "$root/bin/test-native-docker.sh"; fi
	if [[ $mode == all ]]; then
		baseline=$(cd "$root" && python3 bin/buildinfo.py json)
		for part in build-standard build-solar-router native; do
			bash "$0" "$part"
			current=$(cd "$root" && python3 bin/buildinfo.py json)
			if [[ $current != "$baseline" ]]; then
				echo 'Sources changed during validation; rerun all checks on one source state.' >&2
				exit 2
			fi
		done
		echo 'Heltec CI: PASS (both firmware profiles and all required native suites)'
		exit 0
	fi
	work_base=${HELTEC_CI_WORK_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/heltec-ci}
	mkdir -p "$work_base"
	work_base=$(cd "$work_base" && pwd)
	run_dir=$(mktemp -d "$work_base/$mode.XXXXXXXX")
	report_base=$root/.pio/ci/$mode
	mkdir -p "$report_base"
	report=$(mktemp -d "$report_base/run-XXXXXXXX")
	output=${HELTEC_CI_ARTIFACT_DIR:-$report}
	mkdir -p "$report" "$output"
	python3 "$root/bin/ci-snapshot.py" "$root" "$run_dir/workspace" >"$report/source.json"
	printf '%s\n' "$run_dir/workspace" >"$report/workspace-path.txt"
	workspace=$run_dir/workspace
	common=(--rm --platform linux/amd64 --network "${HELTEC_DOCKER_NETWORK:-bridge}"
		--env GIT_CONFIG_COUNT=2
		--env GIT_CONFIG_KEY_0=safe.directory --env GIT_CONFIG_VALUE_0=/workspace
		--env GIT_CONFIG_KEY_1=safe.directory --env GIT_CONFIG_VALUE_1=/workspace/protobufs
		--volume "$workspace:/workspace" --workdir /workspace)
	finish() {
		docker run "${common[@]}" --entrypoint bash "$HELTEC_BUILD_IMAGE" \
			-c 'chown -hR "$1:$2" /workspace' ci-cleanup "$(id -u)" "$(id -g)"
		[[ $run_dir == "$work_base/$mode."* && ! -L $run_dir ]]
		rm -rf -- "$run_dir"
		printf '%s\n' "$report" >"$report_base/latest.txt"
		echo "Reports: $report"
	}
	docker pull "$HELTEC_BUILD_IMAGE"
	docker image inspect "$HELTEC_BUILD_IMAGE" --format '{{.Id}}' >"$report/build-image.txt"
	docker run "${common[@]}" --entrypoint bash "$HELTEC_BUILD_IMAGE" \
		-c 'set -e; python3 --version; pio --version; g++ --version; cat /etc/os-release' >"$report/tool-versions.txt"
	docker run "${common[@]}" --entrypoint bash "$HELTEC_BUILD_IMAGE" \
		bin/ci-host-checks.sh 2>&1 | tee "$report/host-checks.log"
	if [[ $mode == check ]]; then
		finish
		exit 0
	fi
	profile=${mode/build-/heltec-v4-}
	docker run "${common[@]}" \
		--env GITHUB_ACTIONS=true --env PLATFORMIO_BUILD_DIR=/workspace/.pio/build \
		--env MT_ENV="$profile" --env MT_PLATFORM=esp32s3 --env MT_TARGET=build --env MT_VERBOSE=0 \
		"$HELTEC_BUILD_IMAGE" 2>&1 | tee "$report/build.log"
	docker run "${common[@]}" --entrypoint bash "$HELTEC_BUILD_IMAGE" \
		bin/ci-ota.sh 2>&1 | tee "$report/ota.log"
	python3 "$root/bin/ci-verify-artifacts.py" "$workspace" "$profile" \
		"$(git -C "$root" config --get remote.origin.url)" | tee "$report/artifact-validation.json"
	mkdir -p "$output/release"
	cp -a "$workspace/release/." "$output/release/"
	echo "Artifacts: $output/release"
	finish
}
main "$@"
