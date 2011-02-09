CCode - An autocompletion daemon for the C programming language.

1. Linux only, don't ask me to port it somewhere. If you need that - do it.
   But kinda works on Mac too.
2. Relies on a C99 compliance (flexible array members, snprintf behaviour, etc).
3. Mostly done, but has few quirks.
4. Can be used to complete C++/ObjC, but I'm not targeting these languages.
   Don't report C++/ObjC specific bugs.
5. Currently only per directory CFLAGS configuration (just dump your CFLAGS to .ccode file).


Something that should look like a usage guide
----------------------------------------------

1. Build everything (see update.bash).
2. Place it somewhere on your $PATH.
3. Copy vim plugin to your .vim/plugin dir.
4. Daemon starts automatically, everything should work out of the box.
5. Use <C-x><C-o> for autocompletion.

FAQ
---
Q: My linux distribution contains broken LLVM/clang build (e.g. archlinux,
   gentoo) and clang doesn't see its include directory (/usr/lib/clang/2.8/include).
   What should I do?

A: In your project dir:
   echo " -I/usr/lib/clang/2.8/include" >> .ccode
   (Also for archlinux see this bug: https://bugs.archlinux.org/task/22799)