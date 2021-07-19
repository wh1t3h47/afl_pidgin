# afl_pidgin: Hacking pidgin

<div align="center">
  <img alt="Markdown" src="https://img.shields.io/badge/markdown-%23000000.svg?style=for-the-badge&logo=markdown&logoColor=white"/>
  <img alt="Shell Script" src="https://img.shields.io/badge/shell_script-%23121011.svg?style=for-the-badge&logo=gnu-bash&logoColor=white"/>
  <img alt="CMake" src="https://img.shields.io/badge/CMake-%23008FBA.svg?style=for-the-badge&logo=cmake&logoColor=white"/>
  <img alt="Vim" src="https://img.shields.io/badge/VIM-%2311AB00.svg?style=for-the-badge&logo=vim&logoColor=white"/>
  <img alt="C" src="https://img.shields.io/badge/c-%2300599C.svg?style=for-the-badge&logo=c&logoColor=white"/>
</div>

> By wh1t3h47 (Antonio Martos Harres) - github.com/wh1t3h47


Fuzz pidgin via dbus by using AFL++ instrumentation (clang)

This project was just a test to see if I could handle AFL++ and had enough knowledge to fuzz the Pidgin messenger, it's by no means a complete approach, but still a successful one.

I coded this in about a day and got AFL++ running with 8 threads, it fuzzed for about 8 hours (my computer couldn't handle anymore and was forcefully shut down, not even sysrq would work)

It took me about 180 lines of C code and 100 more of shell, I used argv fuzzer to mutate dbus data and wrote a small wrapper to forward dbus messages from argv.

## Warning
> The fuzzing proccess is very resource intensive, ASAN is very memory hungry (afl even recommends limiting it), so this can halt your machine, please save all your work and **be aware** that your machine may lag


## Building
```bash
./build_pidgin.sh
```

## Fuzzing
```bash
sleep 3 && ./afl_start.sh
```

