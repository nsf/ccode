#!/bin/bash
clang -o ccode -L$(llvm-config --libdir) -lclang client.c server.c misc.c main.c strstr.c tpl.c proto.c
cp ccode ~/bin

