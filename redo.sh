#!/bin/bash

g++ \
	-frounding-math \
	-ggdb -O0 \
	-lxcb -lxcb-errors -lxcb-keysyms -lxcb-composite -lxcb-image -lxcb-render \
	-I external/* -I include \
	./src/* \
	-o dragon-shooter

