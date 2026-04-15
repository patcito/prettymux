#!/usr/bin/env bash
#
# verify-strip-layout.sh — Docker + Wayland smoke verification
# for PrettyMux user-visible strip layout work.
#
# Local runs prefer sharing the host Wayland session so the container launches
# a real PrettyMux window on the current desktop. CI/no-host-Wayland falls back
# to a headless Weston compositor.
#
set -euo pipefail

report_failure() {
  local rc=$?
  if [[ "$rc" -ne 0 ]]; then
    echo "verify-strip-layout.sh failed at line ${BASH_LINENO[0]:-unknown}: ${BASH_COMMAND:-unknown}" >&2
  fi
  exit "$rc"
}

trap report_failure EXIT

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GHOSTTY_ROOT="${GHOSTTY_ROOT:-$REPO_ROOT/../ghostty}"
IMAGE_NAME="${IMAGE_NAME:-prettymux-strip-verify}"
RUN_MESON_TESTS="${PRETTYMUX_VERIFY_RUN_TESTS:-0}"
RUN_STRIP_EXERCISE="${PRETTYMUX_VERIFY_STRIP:-1}"
RUN_STRIP_PERSISTENCE_EXERCISE="${PRETTYMUX_VERIFY_STRIP_PERSISTENCE:-1}"
RUN_MULTI_INSTANCE_EXERCISE="${PRETTYMUX_VERIFY_MULTI_INSTANCE:-1}"
RUN_STATUS_EXERCISE="${PRETTYMUX_VERIFY_STATUS:-1}"

if [[ ! -d "$REPO_ROOT/src/gtk" ]]; then
  echo "Repository root not found: $REPO_ROOT" >&2
  exit 1
fi

if [[ ! -f "$GHOSTTY_ROOT/zig-out/lib/libghostty.so" ]]; then
  echo "Ghostty shared library not found at $GHOSTTY_ROOT/zig-out/lib/libghostty.so" >&2
  exit 1
fi

docker_extra_args=()
docker_tty_args=()
runtime_mode="weston"
container_runtime_dir="/tmp/xdg-runtime"
container_wayland_display="wayland-1"

if [[ -n "${WAYLAND_DISPLAY:-}" ]] && [[ -n "${XDG_RUNTIME_DIR:-}" ]] && [[ -S "${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}" ]]; then
  runtime_mode="host-wayland"
  container_runtime_dir="$XDG_RUNTIME_DIR"
  container_wayland_display="$WAYLAND_DISPLAY"
  docker_extra_args+=(
    --user "$(id -u):$(id -g)"
    -e "WAYLAND_DISPLAY=$container_wayland_display"
    -e "XDG_RUNTIME_DIR=$container_runtime_dir"
    -e "XDG_SESSION_TYPE=wayland"
    -v "${XDG_RUNTIME_DIR}:${XDG_RUNTIME_DIR}"
  )
  if [[ -e /dev/dri ]]; then
    docker_extra_args+=(--device /dev/dri)
  fi
else
  docker_extra_args+=(
    -e "WAYLAND_DISPLAY=$container_wayland_display"
    -e "XDG_RUNTIME_DIR=$container_runtime_dir"
    -e "XDG_SESSION_TYPE=wayland"
  )
fi

if [[ -t 0 && -t 1 ]]; then
  docker_tty_args=(-it)
fi

echo "=== Building Docker image ==="
docker build -t "$IMAGE_NAME" \
  -f "$SCRIPT_DIR/debian-bookworm.Dockerfile" \
  "$SCRIPT_DIR"

echo "=== Running PrettyMux verification inside container ==="
docker run --rm \
  "${docker_tty_args[@]}" \
  "${docker_extra_args[@]}" \
  -e "PRETTYMUX_VERIFY_RUNTIME=$runtime_mode" \
  -e "PRETTYMUX_VERIFY_REPO=/workspace" \
  -e "PRETTYMUX_VERIFY_GHOSTTY=/ghostty" \
  -e "RUN_MESON_TESTS=$RUN_MESON_TESTS" \
  -e "RUN_STRIP_EXERCISE=$RUN_STRIP_EXERCISE" \
  -e "RUN_STRIP_PERSISTENCE_EXERCISE=$RUN_STRIP_PERSISTENCE_EXERCISE" \
  -e "RUN_MULTI_INSTANCE_EXERCISE=$RUN_MULTI_INSTANCE_EXERCISE" \
  -e "RUN_STATUS_EXERCISE=$RUN_STATUS_EXERCISE" \
  -e "HOME=/tmp/prettymux-home" \
  -v "$REPO_ROOT:/workspace" \
  -v "$GHOSTTY_ROOT:/ghostty" \
  -w /tmp \
  "$IMAGE_NAME" \
  bash -lc '
    set -euo pipefail

    report_failure() {
      local rc=$?
      if [[ "$rc" -ne 0 ]]; then
        echo "container verification failed at line ${BASH_LINENO[0]:-unknown}: ${BASH_COMMAND:-unknown}" >&2
      fi
      exit "$rc"
    }

    trap report_failure EXIT

    export HOME=/tmp/prettymux-home
    mkdir -p "$HOME" /tmp/build
    cd /tmp/build

    echo "--- Meson setup ---"
    meson setup builddir "$PRETTYMUX_VERIFY_REPO/src/gtk" --prefix=/usr -Dghostty_dir="$PRETTYMUX_VERIFY_GHOSTTY"

    echo "--- Build prettymux + prettymux-open ---"
    ninja -C builddir prettymux prettymux-open

    if [[ "${RUN_MESON_TESTS:-0}" = "1" ]]; then
      echo "--- Run unit tests ---"
      meson test -C builddir --print-errorlogs
    fi

    if [[ "$PRETTYMUX_VERIFY_RUNTIME" = "weston" ]]; then
      echo "--- Starting fallback Weston compositor ---"
      mkdir -p "$XDG_RUNTIME_DIR"
      weston --backend=headless-backend.so --socket="$WAYLAND_DISPLAY" --idle-time=0 --no-config >/tmp/weston.log 2>&1 &
      WESTON_PID=$!
      sleep 2
      if ! kill -0 "$WESTON_PID" 2>/dev/null; then
        echo "Weston failed to start" >&2
        cat /tmp/weston.log >&2 || true
        exit 1
      fi
    fi

    start_prettymux_instance() {
      local out_var="$1"
      local log_path="$2"
      local clean_stale="${3:-0}"
      local instance_id="${4:-}"
      local child_pid=""

      if [[ "$clean_stale" = "1" ]]; then
        rm -f /tmp/prettymux-*.sock
      fi

      if [[ -n "$instance_id" ]]; then
        PRETTYMUX_INSTANCE_ID="$instance_id" \
          LD_LIBRARY_PATH="$PRETTYMUX_VERIFY_GHOSTTY/zig-out/lib" \
          ./builddir/prettymux >"$log_path" 2>&1 &
      else
        LD_LIBRARY_PATH="$PRETTYMUX_VERIFY_GHOSTTY/zig-out/lib" \
          ./builddir/prettymux >"$log_path" 2>&1 &
      fi
      child_pid="$!"
      printf -v "$out_var" '%s' "$child_pid"
    }

    start_nested_prettymux_child() {
      local out_var="$1"
      local log_path="$2"
      local parent_instance_id="$3"
      local terminal_lane_id="$4"
      local child_instance_lane_id="${5:-}"
      local child_pid=""

      if [[ -n "$child_instance_lane_id" ]]; then
        PRETTYMUX=1 \
        PRETTYMUX_SOCKET="/tmp/prettymux-${parent_instance_id}.sock" \
        PRETTYMUX_INSTANCE_ID="$parent_instance_id" \
        PRETTYMUX_TERMINAL_ID="$terminal_lane_id" \
        PRETTYMUX_CHILD_INSTANCE_ID="$child_instance_lane_id" \
        LD_LIBRARY_PATH="$PRETTYMUX_VERIFY_GHOSTTY/zig-out/lib" \
        ./builddir/prettymux >"$log_path" 2>&1 &
      else
        PRETTYMUX=1 \
        PRETTYMUX_SOCKET="/tmp/prettymux-${parent_instance_id}.sock" \
        PRETTYMUX_INSTANCE_ID="$parent_instance_id" \
        PRETTYMUX_TERMINAL_ID="$terminal_lane_id" \
        LD_LIBRARY_PATH="$PRETTYMUX_VERIFY_GHOSTTY/zig-out/lib" \
        ./builddir/prettymux >"$log_path" 2>&1 &
      fi
      child_pid="$!"
      printf -v "$out_var" '%s' "$child_pid"
    }

    wait_for_prettymux_ready() {
      local pid="$1"
      local log_path="$2"
      local target_instance="${3:-}"
      local ready=0
      local -a open_cmd=("./builddir/prettymux-open" "--list-workspaces")

      if [[ -n "$target_instance" ]]; then
        open_cmd=("./builddir/prettymux-open" "--instance" "$target_instance" "--list-workspaces")
      fi

      for _ in $(seq 1 40); do
        if "${open_cmd[@]}" >/tmp/prettymux-open.out 2>/tmp/prettymux-open.err; then
          ready=1
          break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
          echo "PrettyMux exited early" >&2
          cat "$log_path" >&2 || true
          return 1
        fi
        sleep 0.5
      done

      if [[ "$ready" -ne 1 ]]; then
        echo "PrettyMux socket never became ready" >&2
        cat "$log_path" >&2 || true
        cat /tmp/prettymux-open.err >&2 || true
        return 1
      fi

      if ! jq -e ".status == \"ok\" and (.workspaces | type == \"array\")" \
          /tmp/prettymux-open.out >/dev/null; then
        echo "prettymux-open --list-workspaces did not return an ok response" >&2
        cat /tmp/prettymux-open.out >&2 || true
        return 1
      fi
    }

    wait_for_instance_count() {
      local expected_count="$1"
      local pid_a="$2"
      local pid_b="$3"
      local ready=0
      local instances_json=""

      for _ in $(seq 1 40); do
        if instances_json="$(./builddir/prettymux-open --list-instances 2>/tmp/prettymux-instances.err)"; then
          if printf "%s\n" "$instances_json" \
            | jq -e --argjson expected "$expected_count" \
              ".status == \"ok\" and (.instances | length) >= \$expected" >/dev/null; then
            printf "%s\n" "$instances_json" >/tmp/prettymux-instances.out
            ready=1
            break
          fi
        fi
        if ! kill -0 "$pid_a" 2>/dev/null; then
          echo "Primary PrettyMux instance exited early" >&2
          cat /tmp/prettymux.log >&2 || true
          return 1
        fi
        if [[ -n "$pid_b" ]] && ! kill -0 "$pid_b" 2>/dev/null; then
          echo "Secondary PrettyMux instance exited early" >&2
          cat /tmp/prettymux-2.log >&2 || true
          return 1
        fi
        sleep 0.5
      done

      if [[ "$ready" -ne 1 ]]; then
        echo "--list-instances did not report expected running instances" >&2
        cat /tmp/prettymux-instances.err >&2 || true
        cat /tmp/prettymux-instances.out >&2 || true
        return 1
      fi
    }

    echo "--- Launch PrettyMux ---"
    MAIN_INSTANCE_ID="phase6-main-$$"
    PEER_INSTANCE_ID="phase6-peer-$$"
    CHILD_LANE_A_ID="phase6b-lane-a"
    CHILD_LANE_B_ID="phase6b-lane-b"
    CHILD_SAME_LANE_ID="phase6b-lane-same"
    CHILD_SAME_BASE_ID="${MAIN_INSTANCE_ID}-child-${CHILD_SAME_LANE_ID}"
    CHILD_SAME_SLOT_2_ID="${CHILD_SAME_BASE_ID}-2"
    CHILD_INSTANCE_A_ID=""
    CHILD_INSTANCE_B_ID=""
    start_prettymux_instance PM_PID /tmp/prettymux.log 1 "$MAIN_INSTANCE_ID"
    PM2_PID=""
    PM_CHILD_A_PID=""
    PM_CHILD_B_PID=""

    cleanup() {
      for pid in "${PM_CHILD_B_PID:-}" "${PM_CHILD_A_PID:-}" "${PM2_PID:-}" "${PM_PID:-}"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
          kill "$pid" 2>/dev/null || true
          wait "$pid" 2>/dev/null || true
        fi
      done
      if [[ -n "${WESTON_PID:-}" ]] && kill -0 "$WESTON_PID" 2>/dev/null; then
        kill "$WESTON_PID" 2>/dev/null || true
        wait "$WESTON_PID" 2>/dev/null || true
      fi
    }
    trap cleanup EXIT

    echo "--- Wait for live instance ---"
    wait_for_prettymux_ready "$PM_PID" /tmp/prettymux.log "$MAIN_INSTANCE_ID"

    echo "PASS: PrettyMux launched and prettymux-open reached a live instance"
    cat /tmp/prettymux-open.out || true

    assert_layout_response() {
      local context="$1"
      local expected_layout="$2"
      local json="$3"

      if ! printf "%s\n" "$json" \
        | jq -e --arg expected "$expected_layout" \
          ".status == \"ok\" and .layout == \$expected" >/dev/null; then
        echo "$context returned an unexpected response" >&2
        printf "%s\n" "$json" >&2
        exit 1
      fi
    }

    assert_action_ok() {
      local context="$1"
      local json="$2"

      if ! printf "%s\n" "$json" \
        | jq -e ".status == \"ok\"" >/dev/null; then
        echo "$context returned a non-ok action response" >&2
        printf "%s\n" "$json" >&2
        exit 1
      fi
    }

    assert_status_ok() {
      local context="$1"
      local json="$2"

      if ! printf "%s\n" "$json" \
        | jq -e ".status == \"ok\"" >/dev/null; then
        echo "$context returned a non-ok response" >&2
        printf "%s\n" "$json" >&2
        exit 1
      fi
    }

    assert_action_error() {
      local context="$1"
      local json="$2"

      if ! printf "%s\n" "$json" \
        | jq -e ".status == \"error\" and (.message | type == \"string\")" >/dev/null; then
        echo "$context should have returned an explicit action error" >&2
        printf "%s\n" "$json" >&2
        exit 1
      fi
    }

    assert_strip_state_ok() {
      local context="$1"
      local json="$2"

      if ! printf "%s\n" "$json" \
        | jq -e ".status == \"ok\" and .layout == \"strip\" and (.columns | type == \"array\") and (.focusedColumn | type == \"number\")" >/dev/null; then
        echo "$context returned an unexpected strip-state response" >&2
        printf "%s\n" "$json" >&2
        exit 1
      fi
    }

    active_workspace_pane_count() {
      local json="$1"
      printf "%s\n" "$json" \
        | jq -re ".workspaces[] | select(.active == true) | (.panes | length)"
    }

    active_workspace_focused_pane_index() {
      local json="$1"
      printf "%s\n" "$json" \
        | jq -re ".workspaces[] | select(.active == true) | ((.panes | map(.focused) | index(true)) // -1)"
    }

    active_workspace_focused_pane_active_tab() {
      local json="$1"
      printf "%s\n" "$json" \
        | jq -re ".workspaces[] | select(.active == true) | (.panes[] | select(.focused == true) | .activeTab)"
    }

    active_workspace_focused_pane_tab_count() {
      local json="$1"
      printf "%s\n" "$json" \
        | jq -re ".workspaces[] | select(.active == true) | (.panes[] | select(.focused == true) | (.tabs | length))"
    }

    status_commands_exercised=""
    if [[ "${RUN_STATUS_EXERCISE:-0}" = "1" ]]; then
      local_status_entry_id="phase7-status-$PM_PID"
      local_status_entry_id_2="${local_status_entry_id}-detail"

      echo "--- Exercise workspace status commands against live instance ---"

      echo "  --set-workspace-status --id $local_status_entry_id --provider codex --kind session --state running --summary indexing --detail \"indexing repository\" --attention --notify -w 0"
      status_set_json="$(./builddir/prettymux-open --set-workspace-status --id "$local_status_entry_id" --provider codex --kind session --state running --summary indexing --detail "indexing repository" --attention --notify -w 0)"
      printf "%s\n" "$status_set_json"
      if ! printf "%s\n" "$status_set_json" \
        | jq -e --arg id "$local_status_entry_id" \
          ".status == \"ok\" and .workspace == 0 and .entryId == \$id" >/dev/null; then
        echo "--set-workspace-status did not return expected acknowledgement" >&2
        exit 1
      fi

      echo "  --set-workspace-status --id $local_status_entry_id_2 --provider claude --kind review --detail \"waiting on review\" -w 0"
      status_set_detail_json="$(./builddir/prettymux-open --set-workspace-status --id "$local_status_entry_id_2" --provider claude --kind review --detail "waiting on review" -w 0)"
      printf "%s\n" "$status_set_detail_json"
      if ! printf "%s\n" "$status_set_detail_json" \
        | jq -e --arg id "$local_status_entry_id_2" \
          ".status == \"ok\" and .workspace == 0 and .entryId == \$id" >/dev/null; then
        echo "--set-workspace-status detail-only update did not return expected acknowledgement" >&2
        exit 1
      fi

      echo "  --list-workspace-status -w 0"
      status_list_json="$(./builddir/prettymux-open --list-workspace-status -w 0)"
      printf "%s\n" "$status_list_json"
      if ! printf "%s\n" "$status_list_json" \
        | jq -e --arg id1 "$local_status_entry_id" --arg id2 "$local_status_entry_id_2" \
          ".status == \"ok\" and .workspace == 0 and (.entries | any(.entryId == \$id1)) and (.entries | any(.entryId == \$id2))" >/dev/null; then
        echo "--list-workspace-status did not include both expected entries" >&2
        exit 1
      fi
      if ! printf "%s\n" "$status_list_json" \
        | jq -e --arg id2 "$local_status_entry_id_2" \
          ".entries[] | select(.entryId == \$id2) | (.summary == \"waiting on review\")" >/dev/null; then
        echo "detail-only status did not normalize to a readable summary" >&2
        exit 1
      fi

      echo "  --clear-workspace-status --id $local_status_entry_id -w 0"
      status_clear_json="$(./builddir/prettymux-open --clear-workspace-status --id "$local_status_entry_id" -w 0)"
      printf "%s\n" "$status_clear_json"
      if ! printf "%s\n" "$status_clear_json" \
        | jq -e --arg id "$local_status_entry_id" \
          ".status == \"ok\" and .workspace == 0 and .entryId == \$id" >/dev/null; then
        echo "--clear-workspace-status did not return expected acknowledgement" >&2
        exit 1
      fi

      echo "  --list-workspace-status -w 0  (confirm clear)"
      status_list_after_clear_json="$(./builddir/prettymux-open --list-workspace-status -w 0)"
      printf "%s\n" "$status_list_after_clear_json"
      if ! printf "%s\n" "$status_list_after_clear_json" \
        | jq -e --arg id "$local_status_entry_id" \
          ".status == \"ok\" and ((.entries | any(.entryId == \$id)) | not)" >/dev/null; then
        echo "--clear-workspace-status did not remove the target entry" >&2
        exit 1
      fi

      status_commands_exercised="yes"
    else
      echo "--- Workspace status exercise skipped (set PRETTYMUX_VERIFY_STATUS=1 to enable) ---"
    fi

    multi_instance_commands_exercised=""
    phase6b_commands_exercised=""
    phase8b_aux_commands_exercised=""
    if [[ "${RUN_MULTI_INSTANCE_EXERCISE:-0}" = "1" ]]; then
      echo "--- Exercise multi-instance routing against live instances ---"

      start_prettymux_instance PM2_PID /tmp/prettymux-2.log 0 "$PEER_INSTANCE_ID"
      wait_for_prettymux_ready "$PM2_PID" /tmp/prettymux-2.log "$PEER_INSTANCE_ID"
      wait_for_instance_count 2 "$PM_PID" "$PM2_PID"

      instances_json="$(cat /tmp/prettymux-instances.out)"
      printf "%s\n" "$instances_json"

      instance_default="$(printf "%s\n" "$instances_json" \
        | jq -re ".defaultInstanceId // (.instances[] | select(.default == true) | .instanceId) // .instances[0].instanceId")"
      instance_other="$(printf "%s\n" "$instances_json" \
        | jq -re --arg default "$instance_default" \
          "[.instances[] | select(.instanceId != \$default) | .instanceId][0] // empty")"
      if [[ -z "$instance_other" ]] || [[ "$instance_default" = "$instance_other" ]]; then
        echo "--list-instances did not return two distinct instance ids" >&2
        exit 1
      fi

      ws_default="phase6-default-$PM_PID"
      ws_other="phase6-other-$PM2_PID"
      ws_default_only="phase6-default-only-$PM2_PID"

      echo "  --instance $instance_default --new-workspace $ws_default"
      create_default_json="$(./builddir/prettymux-open --instance "$instance_default" --new-workspace "$ws_default")"
      printf "%s\n" "$create_default_json"
      assert_action_ok "--instance default --new-workspace" "$create_default_json"

      echo "  --instance $instance_other --new-workspace $ws_other"
      create_other_json="$(./builddir/prettymux-open --instance "$instance_other" --new-workspace "$ws_other")"
      printf "%s\n" "$create_other_json"
      assert_action_ok "--instance other --new-workspace" "$create_other_json"

      echo "  --instance $instance_default --new-workspace $ws_default_only"
      create_default_only_json="$(./builddir/prettymux-open --instance "$instance_default" --new-workspace "$ws_default_only")"
      printf "%s\n" "$create_default_only_json"
      assert_action_ok "--instance default --new-workspace (default marker)" "$create_default_only_json"
      ws_default_only_idx="$(printf "%s\n" "$create_default_only_json" | jq -re ".index")"

      echo "  --instance $instance_default --list-workspaces"
      list_default_json="$(./builddir/prettymux-open --instance "$instance_default" --list-workspaces)"
      printf "%s\n" "$list_default_json"
      if ! printf "%s\n" "$list_default_json" \
        | jq -e --arg name "$ws_default" \
          ".status == \"ok\" and (.workspaces | any(.name == \$name))" >/dev/null; then
        echo "Targeted list-workspaces for default instance is missing expected workspace" >&2
        exit 1
      fi
      if ! printf "%s\n" "$list_default_json" \
        | jq -e --arg name "$ws_other" \
          "(.workspaces | any(.name == \$name)) | not" >/dev/null; then
        echo "Targeted list-workspaces for default instance leaked other instance workspace" >&2
        exit 1
      fi

      echo "  --instance $instance_other --list-workspaces"
      list_other_json="$(./builddir/prettymux-open --instance "$instance_other" --list-workspaces)"
      printf "%s\n" "$list_other_json"
      if ! printf "%s\n" "$list_other_json" \
        | jq -e --arg name "$ws_other" \
          ".status == \"ok\" and (.workspaces | any(.name == \$name))" >/dev/null; then
        echo "Targeted list-workspaces for other instance is missing expected workspace" >&2
        exit 1
      fi
      if ! printf "%s\n" "$list_other_json" \
        | jq -e --arg name "$ws_default_only" \
          "(.workspaces | any(.name == \$name)) | not" >/dev/null; then
        echo "Targeted list-workspaces for other instance leaked default-only workspace" >&2
        exit 1
      fi

      echo "  --instance $instance_default --move-workspace --to-instance $instance_other -w $ws_default_only_idx"
      move_workspace_json="$(./builddir/prettymux-open --instance "$instance_default" --move-workspace --to-instance "$instance_other" -w "$ws_default_only_idx")"
      printf "%s\n" "$move_workspace_json"
      assert_action_ok "--move-workspace to other instance" "$move_workspace_json"
      if ! printf "%s\n" "$move_workspace_json" \
        | jq -e --arg target "$instance_other" \
          ".status == \"ok\" and .targetInstanceId == \$target" >/dev/null; then
        echo "--move-workspace response did not include expected target instance" >&2
        exit 1
      fi

      echo "  --instance $instance_default --list-workspaces  (after move)"
      list_default_after_move_json="$(./builddir/prettymux-open --instance "$instance_default" --list-workspaces)"
      printf "%s\n" "$list_default_after_move_json"
      if ! printf "%s\n" "$list_default_after_move_json" \
        | jq -e --arg name "$ws_default_only" \
          "(.workspaces | any(.name == \$name)) | not" >/dev/null; then
        echo "Moved workspace is still present in source instance" >&2
        exit 1
      fi

      echo "  --instance $instance_other --list-workspaces  (after move)"
      list_other_after_move_json="$(./builddir/prettymux-open --instance "$instance_other" --list-workspaces)"
      printf "%s\n" "$list_other_after_move_json"
      if ! printf "%s\n" "$list_other_after_move_json" \
        | jq -e --arg name "$ws_default_only" \
          ".status == \"ok\" and (.workspaces | any(.name == \$name))" >/dev/null; then
        echo "Moved workspace is missing from destination instance" >&2
        exit 1
      fi

      echo "  --list-workspaces (default instance resolution)"
      list_default_resolution_json="$(./builddir/prettymux-open --list-workspaces)"
      printf "%s\n" "$list_default_resolution_json"
      if ! printf "%s\n" "$list_default_resolution_json" \
        | jq -e --arg name "$ws_default" \
          ".status == \"ok\" and (.workspaces | any(.name == \$name))" >/dev/null; then
        echo "Default prettymux-open resolution did not target the default instance" >&2
        exit 1
      fi

      echo "  PRETTYMUX_INSTANCE_ID=missing prettymux-open --list-workspaces (expect failure)"
      if PRETTYMUX_INSTANCE_ID="phase6-missing-instance" \
        ./builddir/prettymux-open --list-workspaces >/tmp/prettymux-open-missing-instance.out 2>/tmp/prettymux-open-missing-instance.err; then
        echo "prettymux-open unexpectedly succeeded with missing PRETTYMUX_INSTANCE_ID target" >&2
        cat /tmp/prettymux-open-missing-instance.out >&2 || true
        cat /tmp/prettymux-open-missing-instance.err >&2 || true
        exit 1
      fi

      echo "  PRETTYMUX_SOCKET=missing prettymux-open --list-workspaces (expect failure)"
      if PRETTYMUX_SOCKET="/tmp/prettymux-missing-target.sock" \
        ./builddir/prettymux-open --list-workspaces >/tmp/prettymux-open-missing-socket.out 2>/tmp/prettymux-open-missing-socket.err; then
        echo "prettymux-open unexpectedly succeeded with missing PRETTYMUX_SOCKET target" >&2
        cat /tmp/prettymux-open-missing-socket.out >&2 || true
        cat /tmp/prettymux-open-missing-socket.err >&2 || true
        exit 1
      fi

      echo "--- Exercise per-instance session persistence across restart ---"
      phase6b_main_ws="phase6b-main-$PM_PID"
      phase6b_peer_ws="phase6b-peer-$PM2_PID"
      phase6b_child_a_ws="phase6b-child-a-$PM_PID"
      phase6b_child_b_ws="phase6b-child-b-$PM2_PID"
      phase6b_same_child_a_ws="phase6b-child-same-a-$PM_PID"
      phase6b_same_child_b_ws="phase6b-child-same-b-$PM2_PID"

      echo "  --instance $MAIN_INSTANCE_ID --new-workspace $phase6b_main_ws"
      phase6b_main_new_json="$(./builddir/prettymux-open --instance "$MAIN_INSTANCE_ID" --new-workspace "$phase6b_main_ws")"
      printf "%s\n" "$phase6b_main_new_json"
      assert_action_ok "--instance $MAIN_INSTANCE_ID --new-workspace $phase6b_main_ws" "$phase6b_main_new_json"

      echo "  --instance $PEER_INSTANCE_ID --new-workspace $phase6b_peer_ws"
      phase6b_peer_new_json="$(./builddir/prettymux-open --instance "$PEER_INSTANCE_ID" --new-workspace "$phase6b_peer_ws")"
      printf "%s\n" "$phase6b_peer_new_json"
      assert_action_ok "--instance $PEER_INSTANCE_ID --new-workspace $phase6b_peer_ws" "$phase6b_peer_new_json"

      echo "  --instance $MAIN_INSTANCE_ID --quit  (persist main instance session)"
      phase6b_main_quit_json="$(./builddir/prettymux-open --instance "$MAIN_INSTANCE_ID" --quit)"
      printf "%s\n" "$phase6b_main_quit_json"
      assert_action_ok "--instance $MAIN_INSTANCE_ID --quit" "$phase6b_main_quit_json"

      echo "  --instance $PEER_INSTANCE_ID --quit  (persist peer instance session)"
      phase6b_peer_quit_json="$(./builddir/prettymux-open --instance "$PEER_INSTANCE_ID" --quit)"
      printf "%s\n" "$phase6b_peer_quit_json"
      assert_action_ok "--instance $PEER_INSTANCE_ID --quit" "$phase6b_peer_quit_json"

      if [[ -n "${PM_PID:-}" ]]; then
        wait "$PM_PID" 2>/dev/null || true
      fi
      if [[ -n "${PM2_PID:-}" ]]; then
        wait "$PM2_PID" 2>/dev/null || true
      fi
      PM_PID=""
      PM2_PID=""

      echo "  [restart] PRETTYMUX_INSTANCE_ID=$MAIN_INSTANCE_ID prettymux"
      start_prettymux_instance PM_PID /tmp/prettymux.log 0 "$MAIN_INSTANCE_ID"
      wait_for_prettymux_ready "$PM_PID" /tmp/prettymux.log "$MAIN_INSTANCE_ID"

      echo "  [restart] PRETTYMUX_INSTANCE_ID=$PEER_INSTANCE_ID prettymux"
      start_prettymux_instance PM2_PID /tmp/prettymux-2.log 0 "$PEER_INSTANCE_ID"
      wait_for_prettymux_ready "$PM2_PID" /tmp/prettymux-2.log "$PEER_INSTANCE_ID"

      echo "  --instance $MAIN_INSTANCE_ID --list-workspaces  (after restart)"
      phase6b_main_after_restart_json="$(./builddir/prettymux-open --instance "$MAIN_INSTANCE_ID" --list-workspaces)"
      printf "%s\n" "$phase6b_main_after_restart_json"
      if ! printf "%s\n" "$phase6b_main_after_restart_json" \
        | jq -e --arg own "$phase6b_main_ws" --arg peer "$phase6b_peer_ws" \
          ".status == \"ok\" and (.workspaces | any(.name == \$own)) and ((.workspaces | any(.name == \$peer)) | not)" >/dev/null; then
        echo "Main instance session restore leaked or lost per-instance state" >&2
        exit 1
      fi

      echo "  --instance $PEER_INSTANCE_ID --list-workspaces  (after restart)"
      phase6b_peer_after_restart_json="$(./builddir/prettymux-open --instance "$PEER_INSTANCE_ID" --list-workspaces)"
      printf "%s\n" "$phase6b_peer_after_restart_json"
      if ! printf "%s\n" "$phase6b_peer_after_restart_json" \
        | jq -e --arg own "$phase6b_peer_ws" --arg main "$phase6b_main_ws" \
          ".status == \"ok\" and (.workspaces | any(.name == \$own)) and ((.workspaces | any(.name == \$main)) | not)" >/dev/null; then
        echo "Peer instance session restore leaked or lost per-instance state" >&2
        exit 1
      fi

      echo "--- Exercise nested child-lane session isolation across restart ---"
      CHILD_INSTANCE_A_ID="${MAIN_INSTANCE_ID}-child-${CHILD_LANE_A_ID}"
      CHILD_INSTANCE_B_ID="${MAIN_INSTANCE_ID}-child-${CHILD_LANE_B_ID}"

      echo "  [launch nested] PRETTYMUX_INSTANCE_ID=$MAIN_INSTANCE_ID PRETTYMUX_TERMINAL_ID=$CHILD_LANE_A_ID prettymux"
      start_nested_prettymux_child PM_CHILD_A_PID /tmp/prettymux-child-a.log "$MAIN_INSTANCE_ID" "$CHILD_LANE_A_ID"
      wait_for_prettymux_ready "$PM_CHILD_A_PID" /tmp/prettymux-child-a.log "$CHILD_INSTANCE_A_ID"

      echo "  [launch nested] PRETTYMUX_INSTANCE_ID=$MAIN_INSTANCE_ID PRETTYMUX_TERMINAL_ID=$CHILD_LANE_B_ID prettymux"
      start_nested_prettymux_child PM_CHILD_B_PID /tmp/prettymux-child-b.log "$MAIN_INSTANCE_ID" "$CHILD_LANE_B_ID"
      wait_for_prettymux_ready "$PM_CHILD_B_PID" /tmp/prettymux-child-b.log "$CHILD_INSTANCE_B_ID"

      echo "  --list-instances  (nested child ids must both exist and differ)"
      phase6b_instances_json="$(./builddir/prettymux-open --list-instances)"
      printf "%s\n" "$phase6b_instances_json"
      if ! printf "%s\n" "$phase6b_instances_json" \
        | jq -e --arg a "$CHILD_INSTANCE_A_ID" --arg b "$CHILD_INSTANCE_B_ID" \
          ".status == \"ok\" and (.instances | any(.instanceId == \$a)) and (.instances | any(.instanceId == \$b)) and (\$a != \$b)" >/dev/null; then
        echo "Nested child instances did not expose distinct lane-based ids" >&2
        exit 1
      fi

      echo "  --instance $CHILD_INSTANCE_A_ID --new-workspace $phase6b_child_a_ws"
      phase6b_child_a_new_json="$(./builddir/prettymux-open --instance "$CHILD_INSTANCE_A_ID" --new-workspace "$phase6b_child_a_ws")"
      printf "%s\n" "$phase6b_child_a_new_json"
      assert_action_ok "--instance $CHILD_INSTANCE_A_ID --new-workspace $phase6b_child_a_ws" "$phase6b_child_a_new_json"

      echo "  --instance $CHILD_INSTANCE_B_ID --new-workspace $phase6b_child_b_ws"
      phase6b_child_b_new_json="$(./builddir/prettymux-open --instance "$CHILD_INSTANCE_B_ID" --new-workspace "$phase6b_child_b_ws")"
      printf "%s\n" "$phase6b_child_b_new_json"
      assert_action_ok "--instance $CHILD_INSTANCE_B_ID --new-workspace $phase6b_child_b_ws" "$phase6b_child_b_new_json"

      echo "  --instance $CHILD_INSTANCE_A_ID --quit  (persist child lane A)"
      phase6b_child_a_quit_json="$(./builddir/prettymux-open --instance "$CHILD_INSTANCE_A_ID" --quit)"
      printf "%s\n" "$phase6b_child_a_quit_json"
      assert_action_ok "--instance $CHILD_INSTANCE_A_ID --quit" "$phase6b_child_a_quit_json"

      echo "  --instance $CHILD_INSTANCE_B_ID --quit  (persist child lane B)"
      phase6b_child_b_quit_json="$(./builddir/prettymux-open --instance "$CHILD_INSTANCE_B_ID" --quit)"
      printf "%s\n" "$phase6b_child_b_quit_json"
      assert_action_ok "--instance $CHILD_INSTANCE_B_ID --quit" "$phase6b_child_b_quit_json"

      if [[ -n "${PM_CHILD_A_PID:-}" ]]; then
        wait "$PM_CHILD_A_PID" 2>/dev/null || true
      fi
      if [[ -n "${PM_CHILD_B_PID:-}" ]]; then
        wait "$PM_CHILD_B_PID" 2>/dev/null || true
      fi
      PM_CHILD_A_PID=""
      PM_CHILD_B_PID=""

      echo "  [restart nested] PRETTYMUX_INSTANCE_ID=$MAIN_INSTANCE_ID PRETTYMUX_TERMINAL_ID=$CHILD_LANE_A_ID prettymux"
      start_nested_prettymux_child PM_CHILD_A_PID /tmp/prettymux-child-a.log "$MAIN_INSTANCE_ID" "$CHILD_LANE_A_ID"
      wait_for_prettymux_ready "$PM_CHILD_A_PID" /tmp/prettymux-child-a.log "$CHILD_INSTANCE_A_ID"

      echo "  [restart nested] PRETTYMUX_INSTANCE_ID=$MAIN_INSTANCE_ID PRETTYMUX_TERMINAL_ID=$CHILD_LANE_B_ID prettymux"
      start_nested_prettymux_child PM_CHILD_B_PID /tmp/prettymux-child-b.log "$MAIN_INSTANCE_ID" "$CHILD_LANE_B_ID"
      wait_for_prettymux_ready "$PM_CHILD_B_PID" /tmp/prettymux-child-b.log "$CHILD_INSTANCE_B_ID"

      echo "  --instance $CHILD_INSTANCE_A_ID --list-workspaces  (after restart)"
      phase6b_child_a_after_restart_json="$(./builddir/prettymux-open --instance "$CHILD_INSTANCE_A_ID" --list-workspaces)"
      printf "%s\n" "$phase6b_child_a_after_restart_json"
      if ! printf "%s\n" "$phase6b_child_a_after_restart_json" \
        | jq -e --arg own "$phase6b_child_a_ws" --arg other "$phase6b_child_b_ws" \
          ".status == \"ok\" and (.workspaces | any(.name == \$own)) and ((.workspaces | any(.name == \$other)) | not)" >/dev/null; then
        echo "Nested child lane A restore leaked sibling child lane state" >&2
        exit 1
      fi

      echo "  --instance $CHILD_INSTANCE_B_ID --list-workspaces  (after restart)"
      phase6b_child_b_after_restart_json="$(./builddir/prettymux-open --instance "$CHILD_INSTANCE_B_ID" --list-workspaces)"
      printf "%s\n" "$phase6b_child_b_after_restart_json"
      if ! printf "%s\n" "$phase6b_child_b_after_restart_json" \
        | jq -e --arg own "$phase6b_child_b_ws" --arg other "$phase6b_child_a_ws" \
          ".status == \"ok\" and (.workspaces | any(.name == \$own)) and ((.workspaces | any(.name == \$other)) | not)" >/dev/null; then
        echo "Nested child lane B restore leaked sibling child lane state" >&2
        exit 1
      fi

      echo "  --instance $CHILD_INSTANCE_A_ID --quit  (final lane-A cleanup)"
      phase6b_child_a_quit_final_json="$(./builddir/prettymux-open --instance "$CHILD_INSTANCE_A_ID" --quit)"
      printf "%s\n" "$phase6b_child_a_quit_final_json"
      assert_action_ok "--instance $CHILD_INSTANCE_A_ID --quit (final)" "$phase6b_child_a_quit_final_json"

      echo "  --instance $CHILD_INSTANCE_B_ID --quit  (final lane-B cleanup)"
      phase6b_child_b_quit_final_json="$(./builddir/prettymux-open --instance "$CHILD_INSTANCE_B_ID" --quit)"
      printf "%s\n" "$phase6b_child_b_quit_final_json"
      assert_action_ok "--instance $CHILD_INSTANCE_B_ID --quit (final)" "$phase6b_child_b_quit_final_json"

      if [[ -n "${PM_CHILD_A_PID:-}" ]]; then
        wait "$PM_CHILD_A_PID" 2>/dev/null || true
      fi
      if [[ -n "${PM_CHILD_B_PID:-}" ]]; then
        wait "$PM_CHILD_B_PID" 2>/dev/null || true
      fi
      PM_CHILD_A_PID=""
      PM_CHILD_B_PID=""

      echo "--- Exercise same-lane child isolation across restart ---"
      echo "  [launch nested] PRETTYMUX_INSTANCE_ID=$MAIN_INSTANCE_ID PRETTYMUX_TERMINAL_ID=$CHILD_SAME_LANE_ID prettymux  (expect $CHILD_SAME_BASE_ID)"
      start_nested_prettymux_child PM_CHILD_A_PID /tmp/prettymux-child-same-a.log "$MAIN_INSTANCE_ID" "$CHILD_SAME_LANE_ID"
      wait_for_prettymux_ready "$PM_CHILD_A_PID" /tmp/prettymux-child-same-a.log "$CHILD_SAME_BASE_ID"

      echo "  [launch nested] PRETTYMUX_INSTANCE_ID=$MAIN_INSTANCE_ID PRETTYMUX_TERMINAL_ID=$CHILD_SAME_LANE_ID prettymux  (expect $CHILD_SAME_SLOT_2_ID)"
      start_nested_prettymux_child PM_CHILD_B_PID /tmp/prettymux-child-same-b.log "$MAIN_INSTANCE_ID" "$CHILD_SAME_LANE_ID"
      wait_for_prettymux_ready "$PM_CHILD_B_PID" /tmp/prettymux-child-same-b.log "$CHILD_SAME_SLOT_2_ID"

      echo "  --list-instances  (same-lane children must auto-allocate distinct ids)"
      phase6b_same_lane_instances_json="$(./builddir/prettymux-open --list-instances)"
      printf "%s\n" "$phase6b_same_lane_instances_json"
      if ! printf "%s\n" "$phase6b_same_lane_instances_json" \
        | jq -e --arg a "$CHILD_SAME_BASE_ID" --arg b "$CHILD_SAME_SLOT_2_ID" \
          ".status == \"ok\" and (.instances | any(.instanceId == \$a)) and (.instances | any(.instanceId == \$b)) and (\$a != \$b)" >/dev/null; then
        echo "Same-lane child ids were not auto-assigned as distinct slots" >&2
        exit 1
      fi

      echo "  --instance $CHILD_SAME_BASE_ID --new-workspace $phase6b_same_child_a_ws"
      phase6b_same_child_a_new_json="$(./builddir/prettymux-open --instance "$CHILD_SAME_BASE_ID" --new-workspace "$phase6b_same_child_a_ws")"
      printf "%s\n" "$phase6b_same_child_a_new_json"
      assert_action_ok "--instance $CHILD_SAME_BASE_ID --new-workspace $phase6b_same_child_a_ws" "$phase6b_same_child_a_new_json"

      echo "  --instance $CHILD_SAME_SLOT_2_ID --new-workspace $phase6b_same_child_b_ws"
      phase6b_same_child_b_new_json="$(./builddir/prettymux-open --instance "$CHILD_SAME_SLOT_2_ID" --new-workspace "$phase6b_same_child_b_ws")"
      printf "%s\n" "$phase6b_same_child_b_new_json"
      assert_action_ok "--instance $CHILD_SAME_SLOT_2_ID --new-workspace $phase6b_same_child_b_ws" "$phase6b_same_child_b_new_json"

      echo "  --instance $CHILD_SAME_SLOT_2_ID --quit  (persist slot-2 child while base child stays live)"
      phase6b_same_child_b_quit_json="$(./builddir/prettymux-open --instance "$CHILD_SAME_SLOT_2_ID" --quit)"
      printf "%s\n" "$phase6b_same_child_b_quit_json"
      assert_action_ok "--instance $CHILD_SAME_SLOT_2_ID --quit (occupancy change)" "$phase6b_same_child_b_quit_json"
      if [[ -n "${PM_CHILD_B_PID:-}" ]]; then
        wait "$PM_CHILD_B_PID" 2>/dev/null || true
      fi
      PM_CHILD_B_PID=""

      echo "  [restart nested] PRETTYMUX_INSTANCE_ID=$MAIN_INSTANCE_ID PRETTYMUX_TERMINAL_ID=$CHILD_SAME_LANE_ID prettymux  (slot-2 must be reused while base slot is occupied)"
      start_nested_prettymux_child PM_CHILD_B_PID /tmp/prettymux-child-same-b.log "$MAIN_INSTANCE_ID" "$CHILD_SAME_LANE_ID"
      wait_for_prettymux_ready "$PM_CHILD_B_PID" /tmp/prettymux-child-same-b.log "$CHILD_SAME_SLOT_2_ID"

      echo "  --instance $CHILD_SAME_SLOT_2_ID --list-workspaces  (after occupancy-change restart)"
      phase6b_same_child_b_after_occupancy_restart_json="$(./builddir/prettymux-open --instance "$CHILD_SAME_SLOT_2_ID" --list-workspaces)"
      printf "%s\n" "$phase6b_same_child_b_after_occupancy_restart_json"
      if ! printf "%s\n" "$phase6b_same_child_b_after_occupancy_restart_json" \
        | jq -e --arg own "$phase6b_same_child_b_ws" --arg other "$phase6b_same_child_a_ws" \
          ".status == \"ok\" and (.workspaces | any(.name == \$own)) and ((.workspaces | any(.name == \$other)) | not)" >/dev/null; then
        echo "Same-lane slot-2 child lost or leaked state after occupancy-change restart" >&2
        exit 1
      fi

      echo "  --instance $CHILD_SAME_SLOT_2_ID --quit  (persist slot-2 before full restart)"
      phase6b_same_child_b_quit_again_json="$(./builddir/prettymux-open --instance "$CHILD_SAME_SLOT_2_ID" --quit)"
      printf "%s\n" "$phase6b_same_child_b_quit_again_json"
      assert_action_ok "--instance $CHILD_SAME_SLOT_2_ID --quit (post-occupancy restart)" "$phase6b_same_child_b_quit_again_json"
      if [[ -n "${PM_CHILD_B_PID:-}" ]]; then
        wait "$PM_CHILD_B_PID" 2>/dev/null || true
      fi
      PM_CHILD_B_PID=""

      echo "  --instance $CHILD_SAME_BASE_ID --quit  (persist base same-lane child)"
      phase6b_same_child_a_quit_json="$(./builddir/prettymux-open --instance "$CHILD_SAME_BASE_ID" --quit)"
      printf "%s\n" "$phase6b_same_child_a_quit_json"
      assert_action_ok "--instance $CHILD_SAME_BASE_ID --quit" "$phase6b_same_child_a_quit_json"
      if [[ -n "${PM_CHILD_A_PID:-}" ]]; then
        wait "$PM_CHILD_A_PID" 2>/dev/null || true
      fi
      PM_CHILD_A_PID=""

      echo "  [restart nested] PRETTYMUX_INSTANCE_ID=$MAIN_INSTANCE_ID PRETTYMUX_TERMINAL_ID=$CHILD_SAME_LANE_ID prettymux  (restore base slot)"
      start_nested_prettymux_child PM_CHILD_A_PID /tmp/prettymux-child-same-a.log "$MAIN_INSTANCE_ID" "$CHILD_SAME_LANE_ID"
      wait_for_prettymux_ready "$PM_CHILD_A_PID" /tmp/prettymux-child-same-a.log "$CHILD_SAME_BASE_ID"

      echo "  [restart nested] PRETTYMUX_INSTANCE_ID=$MAIN_INSTANCE_ID PRETTYMUX_TERMINAL_ID=$CHILD_SAME_LANE_ID prettymux  (restore slot-2)"
      start_nested_prettymux_child PM_CHILD_B_PID /tmp/prettymux-child-same-b.log "$MAIN_INSTANCE_ID" "$CHILD_SAME_LANE_ID"
      wait_for_prettymux_ready "$PM_CHILD_B_PID" /tmp/prettymux-child-same-b.log "$CHILD_SAME_SLOT_2_ID"

      echo "  --instance $CHILD_SAME_BASE_ID --list-workspaces  (after restart)"
      phase6b_same_child_a_after_restart_json="$(./builddir/prettymux-open --instance "$CHILD_SAME_BASE_ID" --list-workspaces)"
      printf "%s\n" "$phase6b_same_child_a_after_restart_json"
      if ! printf "%s\n" "$phase6b_same_child_a_after_restart_json" \
        | jq -e --arg own "$phase6b_same_child_a_ws" --arg other "$phase6b_same_child_b_ws" \
          ".status == \"ok\" and (.workspaces | any(.name == \$own)) and ((.workspaces | any(.name == \$other)) | not)" >/dev/null; then
        echo "Same-lane base child restore leaked sibling state" >&2
        exit 1
      fi

      echo "  --instance $CHILD_SAME_SLOT_2_ID --list-workspaces  (after restart)"
      phase6b_same_child_b_after_restart_json="$(./builddir/prettymux-open --instance "$CHILD_SAME_SLOT_2_ID" --list-workspaces)"
      printf "%s\n" "$phase6b_same_child_b_after_restart_json"
      if ! printf "%s\n" "$phase6b_same_child_b_after_restart_json" \
        | jq -e --arg own "$phase6b_same_child_b_ws" --arg other "$phase6b_same_child_a_ws" \
          ".status == \"ok\" and (.workspaces | any(.name == \$own)) and ((.workspaces | any(.name == \$other)) | not)" >/dev/null; then
        echo "Same-lane slot-2 child restore leaked sibling state" >&2
        exit 1
      fi

      echo "  --instance $CHILD_SAME_BASE_ID --quit  (final same-lane base cleanup)"
      phase6b_same_child_a_quit_final_json="$(./builddir/prettymux-open --instance "$CHILD_SAME_BASE_ID" --quit)"
      printf "%s\n" "$phase6b_same_child_a_quit_final_json"
      assert_action_ok "--instance $CHILD_SAME_BASE_ID --quit (final)" "$phase6b_same_child_a_quit_final_json"

      echo "  --instance $CHILD_SAME_SLOT_2_ID --quit  (final same-lane slot-2 cleanup)"
      phase6b_same_child_b_quit_final_json="$(./builddir/prettymux-open --instance "$CHILD_SAME_SLOT_2_ID" --quit)"
      printf "%s\n" "$phase6b_same_child_b_quit_final_json"
      assert_action_ok "--instance $CHILD_SAME_SLOT_2_ID --quit (final)" "$phase6b_same_child_b_quit_final_json"

      if [[ -n "${PM_CHILD_A_PID:-}" ]]; then
        wait "$PM_CHILD_A_PID" 2>/dev/null || true
      fi
      if [[ -n "${PM_CHILD_B_PID:-}" ]]; then
        wait "$PM_CHILD_B_PID" 2>/dev/null || true
      fi
      PM_CHILD_A_PID=""
      PM_CHILD_B_PID=""
      phase6b_commands_exercised="yes"

      if [[ -n "${PM2_PID:-}" ]] && kill -0 "$PM2_PID" 2>/dev/null; then
        kill "$PM2_PID" 2>/dev/null || true
        wait "$PM2_PID" 2>/dev/null || true
      fi
      PM2_PID=""
      wait_for_instance_count 1 "$PM_PID" ""

      multi_instance_commands_exercised="yes"
    else
      echo "--- Multi-instance exercise skipped (set PRETTYMUX_VERIFY_MULTI_INSTANCE=1 to enable) ---"
    fi

    strip_commands_exercised=""
    strip_persistence_commands_exercised=""
    if [[ "${RUN_STRIP_EXERCISE:-0}" = "1" ]]; then
      echo "--- Exercise strip-layout commands against live instance ---"

      echo "  --switch-workspace 0  (ensure workspace 0 is active for actions)"
      switch_ws_json="$(./builddir/prettymux-open --switch-workspace 0)"
      printf "%s\n" "$switch_ws_json"
      assert_action_ok "--switch-workspace 0" "$switch_ws_json"

      echo "  --get-layout -w 0  (expect classic)"
      layout_json="$(./builddir/prettymux-open --get-layout -w 0)"
      printf "%s\n" "$layout_json"
      assert_layout_response "--get-layout -w 0" "classic" "$layout_json"

      echo "  --new-workspace phase5-layout-sync  (create second workspace for cross-workspace layout checks)"
      new_ws_json="$(./builddir/prettymux-open --new-workspace phase5-layout-sync)"
      printf "%s\n" "$new_ws_json"
      assert_action_ok "--new-workspace phase5-layout-sync" "$new_ws_json"

      echo "  --set-layout strip  (apply strip mode across all workspaces)"
      set_all_strip_json="$(./builddir/prettymux-open --set-layout strip)"
      printf "%s\n" "$set_all_strip_json"
      if ! printf "%s\n" "$set_all_strip_json" \
        | jq -e ".status == \"ok\" and .layout == \"strip\" and .scope == \"all\" and .workspaceCount >= 2" >/dev/null; then
        echo "--set-layout strip should apply to all workspaces in this instance" >&2
        printf "%s\n" "$set_all_strip_json" >&2
        exit 1
      fi

      echo "  --get-layout -w 0  (expect strip after global apply)"
      layout_json="$(./builddir/prettymux-open --get-layout -w 0)"
      printf "%s\n" "$layout_json"
      assert_layout_response "--get-layout -w 0 (after global strip)" "strip" "$layout_json"

      echo "  --get-layout -w 1  (expect strip after global apply)"
      layout_json="$(./builddir/prettymux-open --get-layout -w 1)"
      printf "%s\n" "$layout_json"
      assert_layout_response "--get-layout -w 1 (after global strip)" "strip" "$layout_json"

      echo "  --set-layout classic  (apply classic mode across all workspaces)"
      set_all_classic_json="$(./builddir/prettymux-open --set-layout classic)"
      printf "%s\n" "$set_all_classic_json"
      if ! printf "%s\n" "$set_all_classic_json" \
        | jq -e ".status == \"ok\" and .layout == \"classic\" and .scope == \"all\" and .workspaceCount >= 2" >/dev/null; then
        echo "--set-layout classic should apply to all workspaces in this instance" >&2
        printf "%s\n" "$set_all_classic_json" >&2
        exit 1
      fi

      echo "  --get-layout -w 0  (expect classic after global apply)"
      layout_json="$(./builddir/prettymux-open --get-layout -w 0)"
      printf "%s\n" "$layout_json"
      assert_layout_response "--get-layout -w 0 (after global classic)" "classic" "$layout_json"

      echo "  --get-layout -w 1  (expect classic after global apply)"
      layout_json="$(./builddir/prettymux-open --get-layout -w 1)"
      printf "%s\n" "$layout_json"
      assert_layout_response "--get-layout -w 1 (after global classic)" "classic" "$layout_json"

      echo "  --switch-workspace 0  (return to workspace 0 for per-workspace strip action checks)"
      switch_ws_json="$(./builddir/prettymux-open --switch-workspace 0)"
      printf "%s\n" "$switch_ws_json"
      assert_action_ok "--switch-workspace 0 (post-global layout checks)" "$switch_ws_json"

      echo "  --set-layout strip -w 0  (switch to strip)"
      set_json="$(./builddir/prettymux-open --set-layout strip -w 0)"
      printf "%s\n" "$set_json"
      assert_layout_response "--set-layout strip -w 0" "strip" "$set_json"

      echo "  --get-layout -w 0  (expect strip)"
      layout_json="$(./builddir/prettymux-open --get-layout -w 0)"
      printf "%s\n" "$layout_json"
      assert_layout_response "--get-layout -w 0" "strip" "$layout_json"

      echo "  --get-layout -w 1  (targeted change should keep workspace 1 classic)"
      layout_json="$(./builddir/prettymux-open --get-layout -w 1)"
      printf "%s\n" "$layout_json"
      assert_layout_response "--get-layout -w 1 (after targeted strip)" "classic" "$layout_json"

      echo "  --set-layout classic -w 0  (switch back)"
      set_json="$(./builddir/prettymux-open --set-layout classic -w 0)"
      printf "%s\n" "$set_json"
      assert_layout_response "--set-layout classic -w 0" "classic" "$set_json"

      echo "  --get-layout -w 0  (expect classic again)"
      layout_json="$(./builddir/prettymux-open --get-layout -w 0)"
      printf "%s\n" "$layout_json"
      assert_layout_response "--get-layout -w 0" "classic" "$layout_json"

      echo "  --set-layout strip -w 0  (back to strip for action tests)"
      set_json="$(./builddir/prettymux-open --set-layout strip -w 0)"
      printf "%s\n" "$set_json"
      assert_layout_response "--set-layout strip -w 0" "strip" "$set_json"

      echo "  --list-tabs  (capture strip baseline)"
      tabs_json="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_json"
      if ! printf "%s\n" "$tabs_json" \
        | jq -e ".status == \"ok\"" >/dev/null; then
        echo "--list-tabs baseline failed" >&2
        printf "%s\n" "$tabs_json" >&2
        exit 1
      fi
      pane_count_before_split="$(active_workspace_pane_count "$tabs_json")"
      focus_before_single_nav="$(active_workspace_focused_pane_index "$tabs_json")"

      echo "  --action pane.focus.right  (single-column strip no-op success)"
      single_focus_right_json="$(./builddir/prettymux-open --action pane.focus.right)"
      printf "%s\n" "$single_focus_right_json"
      assert_action_ok "--action pane.focus.right (single-column)" "$single_focus_right_json"

      tabs_after_single_right="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_after_single_right"
      pane_count_after_single_right="$(active_workspace_pane_count "$tabs_after_single_right")"
      focus_after_single_right="$(active_workspace_focused_pane_index "$tabs_after_single_right")"
      if [[ "$pane_count_after_single_right" -ne "$pane_count_before_split" ]]; then
        echo "single-column pane.focus.right unexpectedly changed strip pane count" >&2
        exit 1
      fi
      if [[ "$focus_after_single_right" -ne "$focus_before_single_nav" ]]; then
        echo "single-column pane.focus.right should keep focus on the only column" >&2
        exit 1
      fi

      echo "  --action pane.focus.left  (single-column strip no-op success)"
      single_focus_left_json="$(./builddir/prettymux-open --action pane.focus.left)"
      printf "%s\n" "$single_focus_left_json"
      assert_action_ok "--action pane.focus.left (single-column)" "$single_focus_left_json"

      tabs_after_single_left="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_after_single_left"
      pane_count_after_single_left="$(active_workspace_pane_count "$tabs_after_single_left")"
      focus_after_single_left="$(active_workspace_focused_pane_index "$tabs_after_single_left")"
      if [[ "$pane_count_after_single_left" -ne "$pane_count_before_split" ]]; then
        echo "single-column pane.focus.left unexpectedly changed strip pane count" >&2
        exit 1
      fi
      if [[ "$focus_after_single_left" -ne "$focus_before_single_nav" ]]; then
        echo "single-column pane.focus.left should keep focus on the only column" >&2
        exit 1
      fi

      echo "  --action split.horizontal  (strip insert column)"
      split_action_json="$(./builddir/prettymux-open --action split.horizontal)"
      printf "%s\n" "$split_action_json"
      assert_action_ok "--action split.horizontal" "$split_action_json"

      tabs_after_split="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_after_split"
      pane_count_after_split="$(active_workspace_pane_count "$tabs_after_split")"
      focus_before_nav="$(active_workspace_focused_pane_index "$tabs_after_split")"
      if [[ "$pane_count_after_split" -ne $((pane_count_before_split + 1)) ]]; then
        echo "split.horizontal did not add a strip column" >&2
        echo "before=$pane_count_before_split after=$pane_count_after_split" >&2
        exit 1
      fi

      echo "  --action pane.focus.left  (navigate back to previous strip column)"
      focus_left_json="$(./builddir/prettymux-open --action pane.focus.left)"
      printf "%s\n" "$focus_left_json"
      assert_action_ok "--action pane.focus.left" "$focus_left_json"

      tabs_after_focus_left="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_after_focus_left"
      focus_after_nav="$(active_workspace_focused_pane_index "$tabs_after_focus_left")"
      if [[ "$focus_after_nav" == "$focus_before_nav" ]]; then
        echo "pane.focus.left did not move focus across strip columns" >&2
        exit 1
      fi

      echo "  --action pane.focus.right  (navigate forward to verify bidirectional movement)"
      focus_right_json="$(./builddir/prettymux-open --action pane.focus.right)"
      printf "%s\n" "$focus_right_json"
      assert_action_ok "--action pane.focus.right" "$focus_right_json"

      tabs_after_focus_right="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_after_focus_right"
      focus_after_right="$(active_workspace_focused_pane_index "$tabs_after_focus_right")"
      if [[ "$focus_after_right" == "$focus_after_nav" ]]; then
        echo "pane.focus.right did not move focus back across strip columns" >&2
        exit 1
      fi

      echo "  --action pane.zoom  (strip maximize active column)"
      zoom_json="$(./builddir/prettymux-open --action pane.zoom)"
      printf "%s\n" "$zoom_json"
      assert_action_ok "--action pane.zoom" "$zoom_json"

      echo "  --action pane.zoom  (strip unmaximize active column)"
      unzoom_json="$(./builddir/prettymux-open --action pane.zoom)"
      printf "%s\n" "$unzoom_json"
      assert_action_ok "--action pane.zoom (second toggle)" "$unzoom_json"

      echo "  --action split.vertical  (strip vertical stacking in column)"
      split_vertical_json="$(./builddir/prettymux-open --action split.vertical)"
      printf "%s\n" "$split_vertical_json"
      assert_action_ok "--action split.vertical (vertical stacking)" "$split_vertical_json"

      strip_state_after_vsplit="$(./builddir/prettymux-open --get-strip-state -w 0)"
      printf "%s\n" "$strip_state_after_vsplit"
      assert_strip_state_ok "--get-strip-state -w 0 (after split.vertical)" "$strip_state_after_vsplit"
      focused_col_after_vsplit="$(printf "%s\n" "$strip_state_after_vsplit" | jq -re ".focusedColumn")"
      vsplit_pane_count="$(printf "%s\n" "$strip_state_after_vsplit" | jq -re --argjson focused_col "$focused_col_after_vsplit" ".columns[\$focused_col].paneCount // 0")"
      if [[ "$vsplit_pane_count" -lt 2 ]]; then
        echo "split.vertical did not stack panes within focused strip column (focusedColumn=$focused_col_after_vsplit paneCount=$vsplit_pane_count)" >&2
        exit 1
      fi
      focused_strip_pane_before_vertical_nav="$(printf "%s\n" "$strip_state_after_vsplit" | jq -re --argjson focused_col "$focused_col_after_vsplit" ".columns[\$focused_col].focusedPane // -1")"
      tabs_before_vertical_nav="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_before_vertical_nav"
      assert_status_ok "--list-tabs (before vertical pane navigation)" "$tabs_before_vertical_nav"
      focus_pane_before_vertical_nav="$(active_workspace_focused_pane_index "$tabs_before_vertical_nav")"
      if [[ "$focus_pane_before_vertical_nav" -lt 0 ]]; then
        echo "tabs.list did not report a focused pane before strip vertical pane navigation" >&2
        exit 1
      fi

      echo "  --action pane.focus.up  (navigate up within vertically stacked panes)"
      focus_up_json="$(./builddir/prettymux-open --action pane.focus.up)"
      printf "%s\n" "$focus_up_json"
      assert_action_ok "--action pane.focus.up" "$focus_up_json"

      strip_state_after_focus_up="$(./builddir/prettymux-open --get-strip-state -w 0)"
      printf "%s\n" "$strip_state_after_focus_up"
      assert_strip_state_ok "--get-strip-state -w 0 (after pane.focus.up)" "$strip_state_after_focus_up"
      focused_col_after_focus_up="$(printf "%s\n" "$strip_state_after_focus_up" | jq -re ".focusedColumn")"
      focused_pane_after_up="$(printf "%s\n" "$strip_state_after_focus_up" | jq -re --argjson focused_col "$focused_col_after_focus_up" ".columns[\$focused_col].focusedPane // -1")"
      if [[ "$focused_col_after_focus_up" -ne "$focused_col_after_vsplit" ]]; then
        echo "pane.focus.up changed focused strip column unexpectedly (before=$focused_col_after_vsplit after=$focused_col_after_focus_up)" >&2
        exit 1
      fi
      if [[ "$focused_pane_after_up" -ne $((focused_strip_pane_before_vertical_nav - 1)) ]]; then
        echo "pane.focus.up did not move focus exactly one pane up in the active strip column (before=$focused_strip_pane_before_vertical_nav after=$focused_pane_after_up)" >&2
        exit 1
      fi
      tabs_after_focus_up="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_after_focus_up"
      assert_status_ok "--list-tabs (after pane.focus.up)" "$tabs_after_focus_up"
      focus_pane_after_focus_up="$(active_workspace_focused_pane_index "$tabs_after_focus_up")"
      if [[ "$focus_pane_after_focus_up" -eq "$focus_pane_before_vertical_nav" ]]; then
        echo "pane.focus.up did not move real GTK focus to a different pane (focused pane index remained $focus_pane_after_focus_up)" >&2
        exit 1
      fi

      echo "  --action pane.focus.down  (navigate down within vertically stacked panes)"
      focus_down_json="$(./builddir/prettymux-open --action pane.focus.down)"
      printf "%s\n" "$focus_down_json"
      assert_action_ok "--action pane.focus.down" "$focus_down_json"

      strip_state_after_focus_down="$(./builddir/prettymux-open --get-strip-state -w 0)"
      printf "%s\n" "$strip_state_after_focus_down"
      assert_strip_state_ok "--get-strip-state -w 0 (after pane.focus.down)" "$strip_state_after_focus_down"
      focused_col_after_focus_down="$(printf "%s\n" "$strip_state_after_focus_down" | jq -re ".focusedColumn")"
      focused_pane_after_down="$(printf "%s\n" "$strip_state_after_focus_down" | jq -re --argjson focused_col "$focused_col_after_focus_down" ".columns[\$focused_col].focusedPane // -1")"
      if [[ "$focused_col_after_focus_down" -ne "$focused_col_after_vsplit" ]]; then
        echo "pane.focus.down changed focused strip column unexpectedly (before=$focused_col_after_vsplit after=$focused_col_after_focus_down)" >&2
        exit 1
      fi
      if [[ "$focused_pane_after_down" -ne "$focused_strip_pane_before_vertical_nav" ]]; then
        echo "pane.focus.down did not restore focus to the original pane in the active strip column (before=$focused_strip_pane_before_vertical_nav after=$focused_pane_after_down)" >&2
        exit 1
      fi
      tabs_after_focus_down="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_after_focus_down"
      assert_status_ok "--list-tabs (after pane.focus.down)" "$tabs_after_focus_down"
      focus_pane_after_focus_down="$(active_workspace_focused_pane_index "$tabs_after_focus_down")"
      if [[ "$focus_pane_after_focus_down" -ne "$focus_pane_before_vertical_nav" ]]; then
        echo "pane.focus.down did not restore real GTK focus to the original pane (before=$focus_pane_before_vertical_nav after=$focus_pane_after_focus_down)" >&2
        exit 1
      fi

      echo "  --action pane.tab.new  (create a second tab in focused strip pane)"
      tab_new_json="$(./builddir/prettymux-open --action pane.tab.new)"
      printf "%s\n" "$tab_new_json"
      assert_action_ok "--action pane.tab.new" "$tab_new_json"

      tabs_after_tab_new="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_after_tab_new"
      assert_status_ok "--list-tabs (after pane.tab.new)" "$tabs_after_tab_new"
      focus_pane_for_tabs="$(active_workspace_focused_pane_index "$tabs_after_tab_new")"
      tab_count_for_focus_pane="$(active_workspace_focused_pane_tab_count "$tabs_after_tab_new")"
      if [[ "$focus_pane_for_tabs" -lt 0 ]]; then
        echo "tabs.list did not report an active focused pane after pane.tab.new" >&2
        exit 1
      fi
      if [[ "$tab_count_for_focus_pane" -lt 2 ]]; then
        echo "pane.tab.new did not create a second tab in the focused strip pane (tabCount=$tab_count_for_focus_pane)" >&2
        exit 1
      fi

      echo "  --select-tab -w 0 -p $focus_pane_for_tabs -t 0  (set deterministic tab baseline)"
      select_tab_json="$(./builddir/prettymux-open --select-tab -w 0 -p "$focus_pane_for_tabs" -t 0)"
      printf "%s\n" "$select_tab_json"
      assert_status_ok "--select-tab -w 0 -p <focused-pane> -t 0" "$select_tab_json"

      tabs_before_tab_next="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_before_tab_next"
      assert_status_ok "--list-tabs (before tab.next)" "$tabs_before_tab_next"
      focus_pane_before_tab_next="$(active_workspace_focused_pane_index "$tabs_before_tab_next")"
      active_tab_before_tab_next="$(active_workspace_focused_pane_active_tab "$tabs_before_tab_next")"
      if [[ "$active_tab_before_tab_next" -ne 0 ]]; then
        echo "--select-tab baseline failed: expected activeTab=0, got $active_tab_before_tab_next" >&2
        exit 1
      fi

      strip_state_before_tab_nav="$(./builddir/prettymux-open --get-strip-state -w 0)"
      printf "%s\n" "$strip_state_before_tab_nav"
      assert_strip_state_ok "--get-strip-state -w 0 (before tab navigation)" "$strip_state_before_tab_nav"
      focused_col_before_tab_nav="$(printf "%s\n" "$strip_state_before_tab_nav" | jq -re ".focusedColumn")"

      echo "  --action tab.next  (switch tab in active strip pane only)"
      tab_next_json="$(./builddir/prettymux-open --action tab.next)"
      printf "%s\n" "$tab_next_json"
      assert_action_ok "--action tab.next" "$tab_next_json"

      tabs_after_tab_next="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_after_tab_next"
      assert_status_ok "--list-tabs (after tab.next)" "$tabs_after_tab_next"
      focus_pane_after_tab_next="$(active_workspace_focused_pane_index "$tabs_after_tab_next")"
      active_tab_after_tab_next="$(active_workspace_focused_pane_active_tab "$tabs_after_tab_next")"
      if [[ "$focus_pane_after_tab_next" -ne "$focus_pane_before_tab_next" ]]; then
        echo "tab.next moved focus to a different pane/column instead of switching tab in the active strip pane (before=$focus_pane_before_tab_next after=$focus_pane_after_tab_next)" >&2
        exit 1
      fi
      if [[ "$active_tab_after_tab_next" -ne 1 ]]; then
        echo "tab.next did not advance the tab index inside the focused strip pane (before=$active_tab_before_tab_next after=$active_tab_after_tab_next)" >&2
        exit 1
      fi

      strip_state_after_tab_next="$(./builddir/prettymux-open --get-strip-state -w 0)"
      printf "%s\n" "$strip_state_after_tab_next"
      assert_strip_state_ok "--get-strip-state -w 0 (after tab.next)" "$strip_state_after_tab_next"
      focused_col_after_tab_next="$(printf "%s\n" "$strip_state_after_tab_next" | jq -re ".focusedColumn")"
      if [[ "$focused_col_after_tab_next" -ne "$focused_col_before_tab_nav" ]]; then
        echo "tab.next changed focused strip column unexpectedly (before=$focused_col_before_tab_nav after=$focused_col_after_tab_next)" >&2
        exit 1
      fi

      echo "  --action tab.prev  (switch back in active strip pane only)"
      tab_prev_json="$(./builddir/prettymux-open --action tab.prev)"
      printf "%s\n" "$tab_prev_json"
      assert_action_ok "--action tab.prev" "$tab_prev_json"

      tabs_after_tab_prev="$(./builddir/prettymux-open --list-tabs)"
      printf "%s\n" "$tabs_after_tab_prev"
      assert_status_ok "--list-tabs (after tab.prev)" "$tabs_after_tab_prev"
      focus_pane_after_tab_prev="$(active_workspace_focused_pane_index "$tabs_after_tab_prev")"
      active_tab_after_tab_prev="$(active_workspace_focused_pane_active_tab "$tabs_after_tab_prev")"
      if [[ "$focus_pane_after_tab_prev" -ne "$focus_pane_before_tab_next" ]]; then
        echo "tab.prev moved focus to a different pane/column instead of switching tab in the active strip pane (before=$focus_pane_before_tab_next after=$focus_pane_after_tab_prev)" >&2
        exit 1
      fi
      if [[ "$active_tab_after_tab_prev" -ne "$active_tab_before_tab_next" ]]; then
        echo "tab.prev did not restore the original tab index inside the focused strip pane (expected=$active_tab_before_tab_next actual=$active_tab_after_tab_prev)" >&2
        exit 1
      fi

      strip_state_after_tab_prev="$(./builddir/prettymux-open --get-strip-state -w 0)"
      printf "%s\n" "$strip_state_after_tab_prev"
      assert_strip_state_ok "--get-strip-state -w 0 (after tab.prev)" "$strip_state_after_tab_prev"
      focused_col_after_tab_prev="$(printf "%s\n" "$strip_state_after_tab_prev" | jq -re ".focusedColumn")"
      if [[ "$focused_col_after_tab_prev" -ne "$focused_col_before_tab_nav" ]]; then
        echo "tab.prev changed focused strip column unexpectedly (before=$focused_col_before_tab_nav after=$focused_col_after_tab_prev)" >&2
        exit 1
      fi

      echo "  --action pane.focus.left  (prepare pane.close on a single-pane strip column)"
      focus_left_before_close_json="$(./builddir/prettymux-open --action pane.focus.left)"
      printf "%s\n" "$focus_left_before_close_json"
      assert_action_ok "--action pane.focus.left (before pane.close)" "$focus_left_before_close_json"

      strip_state_before_close="$(./builddir/prettymux-open --get-strip-state -w 0)"
      printf "%s\n" "$strip_state_before_close"
      assert_strip_state_ok "--get-strip-state -w 0 (before pane.close)" "$strip_state_before_close"
      column_count_before_close="$(printf "%s\n" "$strip_state_before_close" | jq -re ".columns | length")"
      focused_col_before_close="$(printf "%s\n" "$strip_state_before_close" | jq -re ".focusedColumn")"
      focused_col_pane_count_before_close="$(printf "%s\n" "$strip_state_before_close" | jq -re --argjson focused_col "$focused_col_before_close" ".columns[\$focused_col].paneCount // 0")"
      if [[ "$focused_col_pane_count_before_close" -ne 1 ]]; then
        echo "pane.close verification expected focus on a single-pane strip column before close (focusedColumn=$focused_col_before_close paneCount=$focused_col_pane_count_before_close)" >&2
        exit 1
      fi

      echo "  --action pane.close --non-interactive  (strip remove focused column)"
      pane_close_json="$(./builddir/prettymux-open --action pane.close --non-interactive)"
      printf "%s\n" "$pane_close_json"
      assert_action_ok "--action pane.close --non-interactive" "$pane_close_json"

      strip_state_after_close="$(./builddir/prettymux-open --get-strip-state -w 0)"
      printf "%s\n" "$strip_state_after_close"
      assert_strip_state_ok "--get-strip-state -w 0 (after pane.close)" "$strip_state_after_close"
      column_count_after_close="$(printf "%s\n" "$strip_state_after_close" | jq -re ".columns | length")"
      if [[ "$column_count_after_close" -ne $((column_count_before_close - 1)) ]]; then
        echo "pane.close --non-interactive did not remove exactly one strip column" >&2
        echo "before=$column_count_before_close after=$column_count_after_close" >&2
        exit 1
      fi

      if [[ "${RUN_STRIP_PERSISTENCE_EXERCISE:-1}" = "1" ]]; then
        echo "  --set-layout strip -w 0  (prepare deterministic strip persistence state)"
        set_json="$(./builddir/prettymux-open --set-layout strip -w 0)"
        printf "%s\n" "$set_json"
        assert_layout_response "--set-layout strip -w 0 (persistence setup)" "strip" "$set_json"

        echo "  --action split.horizontal  (create second strip column)"
        split2_json="$(./builddir/prettymux-open --action split.horizontal)"
        printf "%s\n" "$split2_json"
        assert_action_ok "--action split.horizontal (persistence setup #1)" "$split2_json"

        echo "  --action split.horizontal  (create third strip column)"
        split3_json="$(./builddir/prettymux-open --action split.horizontal)"
        printf "%s\n" "$split3_json"
        assert_action_ok "--action split.horizontal (persistence setup #2)" "$split3_json"

        echo "  --action pane.zoom  (maximize currently focused strip column)"
        zoom_persist_first_json="$(./builddir/prettymux-open --action pane.zoom)"
        printf "%s\n" "$zoom_persist_first_json"
        assert_action_ok "--action pane.zoom (persistence setup #1)" "$zoom_persist_first_json"

        echo "  --action pane.focus.left  (move focus to previous strip column)"
        focus_left_persist_json="$(./builddir/prettymux-open --action pane.focus.left)"
        printf "%s\n" "$focus_left_persist_json"
        assert_action_ok "--action pane.focus.left (persistence setup)" "$focus_left_persist_json"

        echo "  --action pane.zoom  (maximize second strip column)"
        zoom_persist_second_json="$(./builddir/prettymux-open --action pane.zoom)"
        printf "%s\n" "$zoom_persist_second_json"
        assert_action_ok "--action pane.zoom (persistence setup #2)" "$zoom_persist_second_json"

        echo "  --get-strip-state -w 0  (capture pre-restart strip state)"
        strip_state_before_json="$(./builddir/prettymux-open --get-strip-state -w 0)"
        printf "%s\n" "$strip_state_before_json"
        assert_strip_state_ok "--get-strip-state -w 0 (before restart)" "$strip_state_before_json"
        strip_state_before_norm="$(printf "%s\n" "$strip_state_before_json" | jq -c "{layout, focusedColumn, columns: [.columns[] | {width: (if .maximized then 0 else .width end), maximized}]}")"

        echo "  --quit  (force graceful save before restart)"
        quit_json="$(./builddir/prettymux-open --quit)"
        printf "%s\n" "$quit_json"
        assert_action_ok "--quit" "$quit_json"
        if [[ -n "${PM_PID:-}" ]]; then
          wait "$PM_PID" 2>/dev/null || true
        fi
        PM_PID=

        echo "--- Relaunch PrettyMux to validate restored strip session ---"
        start_prettymux_instance PM_PID /tmp/prettymux.log 1 "$MAIN_INSTANCE_ID"
        wait_for_prettymux_ready "$PM_PID" /tmp/prettymux.log "$MAIN_INSTANCE_ID"
        echo "PASS: PrettyMux relaunched and prettymux-open reached restored instance"
        cat /tmp/prettymux-open.out || true

        echo "  --get-strip-state -w 0  (capture post-restart strip state)"
        strip_state_after_json="$(./builddir/prettymux-open --get-strip-state -w 0)"
        printf "%s\n" "$strip_state_after_json"
        assert_strip_state_ok "--get-strip-state -w 0 (after restart)" "$strip_state_after_json"
        strip_state_after_norm="$(printf "%s\n" "$strip_state_after_json" | jq -c "{layout, focusedColumn, columns: [.columns[] | {width: (if .maximized then 0 else .width end), maximized}]}")"

        if [[ "$strip_state_before_norm" != "$strip_state_after_norm" ]]; then
          echo "strip session restore mismatch across restart" >&2
          echo "before: $strip_state_before_norm" >&2
          echo "after:  $strip_state_after_norm" >&2
          exit 1
        fi

        echo "PASS: strip focused column, widths, and maximize flags restored after restart"

        echo "  --get-layout -w 9999  (expect error for invalid workspace)"
        invalid_layout_json="$(./builddir/prettymux-open --get-layout -w 9999 || true)"
        printf "%s\n" "$invalid_layout_json"
        if ! printf "%s\n" "$invalid_layout_json" \
          | jq -e ".status == \"error\" and .message == \"invalid workspace index\"" >/dev/null; then
          echo "--get-layout -w 9999 should return an invalid workspace error" >&2
          printf "%s\n" "$invalid_layout_json" >&2
          exit 1
        fi

        strip_persistence_commands_exercised="yes"
      fi

      phase8b_aux_commands_exercised="yes"
      strip_commands_exercised="yes"
    else
      echo "--- Strip-layout exercise skipped (set PRETTYMUX_VERIFY_STRIP=1 to enable) ---"
    fi

    echo ""
    echo "=== Verification summary ==="
    echo "Runtime mode: $PRETTYMUX_VERIFY_RUNTIME"
    echo "Wayland display: $WAYLAND_DISPLAY"
    echo "prettymux-open commands exercised:"
    echo "  --list-workspaces"
    if [[ -n "$status_commands_exercised" ]]; then
      echo "  --set-workspace-status --id phase7-status-<pid> --provider codex --kind session --state running --summary indexing --detail \"indexing repository\" --attention --notify -w 0"
      echo "  --set-workspace-status --id phase7-status-<pid>-detail --provider claude --kind review --detail \"waiting on review\" -w 0"
      echo "  --list-workspace-status -w 0"
      echo "  --clear-workspace-status --id phase7-status-<pid> -w 0"
      echo "  --list-workspace-status -w 0"
    fi
    if [[ -n "$multi_instance_commands_exercised" ]]; then
      echo "  --list-instances"
      echo "  --instance <default-id> --new-workspace phase6-default-<pid>"
      echo "  --instance <other-id> --new-workspace phase6-other-<pid>"
      echo "  --instance <default-id> --new-workspace phase6-default-only-<pid>"
      echo "  --instance <default-id> --list-workspaces"
      echo "  --instance <other-id> --list-workspaces"
      echo "  --instance <default-id> --move-workspace --to-instance <other-id> -w <workspace-index>"
      echo "  --instance <default-id> --list-workspaces (after move)"
      echo "  --instance <other-id> --list-workspaces (after move)"
      echo "  --list-workspaces (default instance resolution)"
      echo "  PRETTYMUX_INSTANCE_ID=phase6-missing-instance --list-workspaces (expected failure)"
      echo "  PRETTYMUX_SOCKET=/tmp/prettymux-missing-target.sock --list-workspaces (expected failure)"
    fi
    if [[ -n "$phase6b_commands_exercised" ]]; then
      echo "  --instance phase6-main-<pid> --new-workspace phase6b-main-<pid>"
      echo "  --instance phase6-peer-<pid> --new-workspace phase6b-peer-<pid>"
      echo "  --instance phase6-main-<pid> --quit"
      echo "  --instance phase6-peer-<pid> --quit"
      echo "  [restart prettymux with PRETTYMUX_INSTANCE_ID=phase6-main-<pid>]"
      echo "  [restart prettymux with PRETTYMUX_INSTANCE_ID=phase6-peer-<pid>]"
      echo "  --instance phase6-main-<pid> --list-workspaces (after restart)"
      echo "  --instance phase6-peer-<pid> --list-workspaces (after restart)"
      echo "  [launch nested child with PRETTYMUX_INSTANCE_ID=phase6-main-<pid> PRETTYMUX_TERMINAL_ID=phase6b-lane-a]"
      echo "  [launch nested child with PRETTYMUX_INSTANCE_ID=phase6-main-<pid> PRETTYMUX_TERMINAL_ID=phase6b-lane-b]"
      echo "  --list-instances (assert phase6-main-<pid>-child-phase6b-lane-a and ...-lane-b)"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-a --new-workspace phase6b-child-a-<pid>"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-b --new-workspace phase6b-child-b-<pid>"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-a --quit"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-b --quit"
      echo "  [restart both nested children with same PRETTYMUX_TERMINAL_ID lanes]"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-a --list-workspaces (after restart)"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-b --list-workspaces (after restart)"
      echo "  [launch nested child with PRETTYMUX_INSTANCE_ID=phase6-main-<pid> PRETTYMUX_TERMINAL_ID=phase6b-lane-same] (expect ...-child-phase6b-lane-same)"
      echo "  [launch nested child with PRETTYMUX_INSTANCE_ID=phase6-main-<pid> PRETTYMUX_TERMINAL_ID=phase6b-lane-same] (expect ...-child-phase6b-lane-same-2)"
      echo "  --list-instances (assert phase6-main-<pid>-child-phase6b-lane-same and ...-lane-same-2)"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-same --new-workspace phase6b-child-same-a-<pid>"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-same-2 --new-workspace phase6b-child-same-b-<pid>"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-same-2 --quit (while base child remains running)"
      echo "  [restart nested same-lane child without PRETTYMUX_CHILD_INSTANCE_ID] (must reuse ...-lane-same-2)"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-same-2 --list-workspaces (after occupancy-change restart)"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-same --quit"
      echo "  [restart both same-lane children with default auto-slot assignment]"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-same --list-workspaces (after restart)"
      echo "  --instance phase6-main-<pid>-child-phase6b-lane-same-2 --list-workspaces (after restart)"
    fi
    if [[ -n "$phase8b_aux_commands_exercised" ]]; then
      echo "  [phase8b] --action split.vertical"
      echo "  [phase8b] --get-strip-state -w 0 (assert focused column paneCount >= 2)"
      echo "  [phase8b] --action pane.focus.up / --action pane.focus.down"
      echo "  [phase8b] --get-strip-state -w 0 (assert focusedPane moves ±1 without column drift)"
      echo "  [phase8b] --list-tabs (assert real focused pane index changes/restores)"
      echo "  [phase8b] --action pane.tab.new / --action tab.next / --action tab.prev"
      echo "  [phase8b] --list-tabs + --get-strip-state -w 0 (assert tab switching stays in active column)"
      echo "  [phase8b] note: ports/progress labels are UI-derived from terminal reports and are not queryable via prettymux-open; verify via focused GTK tests"
    fi
    if [[ -n "$strip_commands_exercised" ]]; then
      echo "  --get-layout -w 0"
      echo "  --new-workspace phase5-layout-sync"
      echo "  --set-layout strip"
      echo "  --get-layout -w 0"
      echo "  --get-layout -w 1"
      echo "  --set-layout classic"
      echo "  --get-layout -w 0"
      echo "  --get-layout -w 1"
      echo "  --switch-workspace 0"
      echo "  --set-layout strip -w 0"
      echo "  --get-layout -w 0"
      echo "  --get-layout -w 1"
      echo "  --set-layout classic -w 0"
      echo "  --get-layout -w 0"
      echo "  --set-layout strip -w 0"
      echo "  --list-tabs"
      echo "  --action pane.focus.right (single-column no-op)"
      echo "  --list-tabs"
      echo "  --action pane.focus.left (single-column no-op)"
      echo "  --list-tabs"
      echo "  --action split.horizontal"
      echo "  --action pane.focus.left (after split)"
      echo "  --action pane.focus.right (bidirectional)"
      echo "  --action pane.zoom"
      echo "  --action pane.zoom (second toggle)"
      echo "  --action split.vertical"
      echo "  --get-strip-state -w 0 (after split.vertical)"
      echo "  --list-tabs (before vertical pane navigation)"
      echo "  --action pane.focus.up"
      echo "  --get-strip-state -w 0 (after pane.focus.up)"
      echo "  --list-tabs (after pane.focus.up)"
      echo "  --action pane.focus.down"
      echo "  --get-strip-state -w 0 (after pane.focus.down)"
      echo "  --list-tabs (after pane.focus.down)"
      echo "  --action pane.tab.new"
      echo "  --list-tabs (after pane.tab.new)"
      echo "  --select-tab -w 0 -p <focused-pane> -t 0"
      echo "  --list-tabs (before tab.next)"
      echo "  --get-strip-state -w 0 (before tab navigation)"
      echo "  --action tab.next"
      echo "  --list-tabs (after tab.next)"
      echo "  --get-strip-state -w 0 (after tab.next)"
      echo "  --action tab.prev"
      echo "  --list-tabs (after tab.prev)"
      echo "  --get-strip-state -w 0 (after tab.prev)"
      echo "  --action pane.focus.left (before pane.close)"
      echo "  --get-strip-state -w 0 (before pane.close)"
      echo "  --action pane.close --non-interactive"
      echo "  --get-strip-state -w 0 (after pane.close)"
      if [[ -n "$strip_persistence_commands_exercised" ]]; then
        echo "  --set-layout strip -w 0 (persistence setup)"
        echo "  --action split.horizontal"
        echo "  --action split.horizontal"
        echo "  --action pane.zoom"
        echo "  --action pane.focus.left"
        echo "  --action pane.zoom"
        echo "  --get-strip-state -w 0 (before restart)"
        echo "  --quit"
        echo "  [restart prettymux process]"
        echo "  --list-workspaces"
        echo "  --get-strip-state -w 0 (after restart)"
        echo "  --get-layout -w 9999 (expected error)"
      fi
    fi
  '

trap - EXIT
echo "PASS: verify-strip-layout.sh completed successfully"
