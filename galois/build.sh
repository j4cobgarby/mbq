#!/bin/bash

if ! test -d build; then
	echo "making a new build folder"
	mkdir build
fi

cd build
cmake ..
make sssp-cpu -j4
make ppsp-cpu -j4
make bfs-cpu -j4
make pagerank-push-cpu -j4
