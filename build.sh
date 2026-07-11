#!/bin/bash
g++ -std=c++20 -Wall -Wextra -pedantic \
    midi2.cpp application.cpp looper_fsm.cpp \
    $(pkg-config --cflags --libs fluidsynth) \
    -pthread -o midi_looper
