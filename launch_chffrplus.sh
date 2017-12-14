#!/usr/bin/bash

function launch {
  # apply update
  if [ "$(git rev-parse HEAD)" != "$(git rev-parse @{u})" ]; then
     git reset --hard @{u} &&
     git clean -xdf &&
     exec "${BASH_SOURCE[0]}"
  fi

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
