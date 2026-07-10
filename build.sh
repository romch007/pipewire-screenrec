#!/bin/sh

set -ex

cc -Wall -Wextra screenrec.c -g $(pkg-config --libs --cflags libpipewire-0.3 libportal) -o screenrec
