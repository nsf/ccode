#!/bin/bash
clang -o ccode -L$(llvm-config --libdir) -lclang main.c strstr.c tpl.c proto.c
cp ccode ~/bin

