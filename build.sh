#!/bin/sh
cc -O2 -Wall -Wextra -std=c99 $(pkg-config --cflags gtk+-3.0) -o mktile mktile.c $(pkg-config --libs gtk+-3.0) -lX11
