#!/usr/bin/env bash
# Fail if any tracked file is a video. CI counterpart of .githooks/pre-commit,
# which only runs on machines where core.hooksPath has been enabled.
set -euo pipefail

if ! command -v file >/dev/null 2>&1; then
	echo "error: the 'file' command is required to inspect tracked files" >&2
	exit 1
fi

video_extension_pattern='^(mp4|m4v|mov|avi|mkv|webm|mpeg|mpg|wmv|flv|3gp|3g2|ogv|vob|h264|h265|hevc|mts|m2ts|mxf|y4m|asf|rm|rmvb)$'
blocked=0

while IFS= read -r -d '' path; do
	[[ -f "$path" ]] || continue
	lower_path=${path,,}
	extension=${lower_path##*.}
	mime_type=$(file --brief --mime-type -- "$path")

	if [[ "$extension" =~ $video_extension_pattern || "$mime_type" == video/* ]]; then
		echo "error: video file is tracked by Git: $path ($mime_type)" >&2
		blocked=1
	fi
done < <(git ls-files -z)

if [[ "$blocked" -ne 0 ]]; then
	echo "Keep camera recordings and replay videos local; remove them from Git." >&2
	exit 1
fi

echo "No tracked video files."
