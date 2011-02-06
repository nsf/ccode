#!/bin/bash
clang -o ccode -L/usr/lib/llvm -lclang main.c strstr.c tpl.c proto.c
cp ccode ~/bin

