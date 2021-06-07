# afl_pidgin

Fuzz pidgin via dbus by using AFL++ instrumentation (clang)

This project was just a test to see if I could handle AFL++ and had enough knowledge to fuzz the Pidgin messenger, it's by no means a complete approach, but still a successful one.

I coded this in about a day and got AFL++ running with 8 threads, it fuzzed for about 8 hours (my computer couldn't handle anymore and was forcefully shut down, not even sysrq would work)

It took me about 140 lines of C code and 100 more of shell, I used argv fuzzer to mutate dbus data

## Warning
> The fuzzing proccess is very resource intensive, ASAN is very memory hungry (afl even recommends disabling it), so this can halt your machine, please save all your work and BE AWARE that your machine may lag

