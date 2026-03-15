#!/bin/sh
cc -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function -Wno-unused-parameter -Wno-format-truncation -Wno-stringop-truncation -std=c99 -o mktile mktile.c $(pkg-config --cflags freetype2) -lX11 -lXext $(pkg-config --libs freetype2) -lm
