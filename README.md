# aprepend

A command-line utility to append or prepend data to a stream of data. Compiles for Linux and Windows.

There are two main things of interest in this project:
  + 1. The code is very nice in my opinion. It's very satisfying to read through.
  + 2. It's built to run as fast as reasonably possible (as in without taking up too much memory and being an inconvenience to the user) (although currently there are 2 TODOs open that can make it even faster while staying within the constraints, so it's not as fast as reasonably possible quite yet)

# How To Build

## For Linux
After cloning the repo (non-recursive is okay, there are and will be no submodules), cd into it and run ```make```. This will use clang++-11 with C++20. If you want to try compiling it with an older version of clang and an older C++ version, you can simply change clang++-11 to clang++-X in the makefile. There's only one occurance and the makefile is super simple, so don't worry. As for the language version, you can either change it in the makefile or use the make variable that I've set up: ```make CPP_STD:=c++<put desired version here>```

Also:
  + ```make clean```       --> cleans up ALL untracked files
  + ```make unoptimized``` --> does an unoptimized build

NOTE: The only other thing you need in order to build on Linux besides ```git```, ```make``` and ```clang++-11``` is ```sh```, but every Linux system that I know of has that by default, so that's no big deal.

ALSO: ```git``` isn't only used for cloning the repo. The makefile uses it for ```make clean```, so you do actually need it.

## For Windows
After cloning the repo (non-recursive is okay, there are and will be no submodules), cd into it and run ```build_on_windows.bat```. This will build into the top-level folder in the repo, NOT the bin folder, but all the build products are still untracked, so just ```git clean``` and you can easily remove them.

IMPORTANT: ```build_on_windows.bat``` needs a functioning ```cl.exe``` in the PATH to work, which is most easily accomplished by running the script inside of a Visual Studio Developer Command Prompt instance.

# How To Install
There is nothing to install, just move the final binary to where ever you want it (like a folder in the PATH) and start using it.

# How To Use
Run ```aprepend --help``` for information about usage of the program.
