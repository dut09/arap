#!/bin/bash
# Change files below for different models.
OFF_FILE_NAME=/home/taodu/research/arap/model/decimated-knight.off

cd build
cmake -DCMAKE_BUILD_TYPE=Release ../
make
./demo_bin $OFF_FILE_NAME
