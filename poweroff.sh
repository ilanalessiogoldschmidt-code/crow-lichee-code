#!/bin/bash
set -euo pipefail
exec "$(dirname "$0")/crowctl.sh" poweroff
