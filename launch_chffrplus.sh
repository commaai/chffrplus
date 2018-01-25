#!/usr/bin/bash

function launch {
  # apply update
  if [ "$(git rev-parse HEAD)" != "$(git rev-parse @{u})" ]; then
     git reset --hard @{u} &&
     git clean -xdf &&
     exec "${BASH_SOURCE[0]}"
  fi

  # check if NEOS update is required
  while [ "$(cat /VERSION)" -lt 4 ] && [ ! -e /data/media/0/noupdate ]; do
    curl -o /tmp/updater https://neos.comma.ai/updater && chmod +x /tmp/updater && /tmp/updater
    sleep 10
  done

  # no cpu rationing in chffrplus
  echo 0-3 > /dev/cpuset/background/cpus
  echo 0-3 > /dev/cpuset/system-background/cpus
  echo 0-3 > /dev/cpuset/foreground/boost/cpus
  echo 0-3 > /dev/cpuset/foreground/cpus
  echo 0-3 > /dev/cpuset/android/cpus


  export PYTHONPATH="$PWD"

  # start manager
  cd selfdrive
  PASSIVE=1 ./manager.py

  # if broken, keep on screen error
  while true; do sleep 1; done
}

launch
