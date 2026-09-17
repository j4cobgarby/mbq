#!/bin/bash

input_dir=../../inputs_galois
exec_dir=./build/lonestar/analytics/cpu/bfs

graphs=(USAr LJ TW HYPERH)
startnodes=(1 1 1 2)

galois_algos=(OBIM PMOD)

for t in {48,1}
do
	for algo in ${!galois_algos[@]}
	do 
    		for i in {0..3}
    		do
        		echo "Results" > results_bfs_${t}_${graphs[i]}_${galois_algos[algo]}.log
        		for j in {1..10}
        		do
            			echo $j
            			${exec_dir}/bfs-cpu -startNode ${startnodes[i]} -delta 0 -t $t -algo=${galois_algos[algo]} ${input_dir}/${graphs[i]}.gr >> results_bfs_${t}_${graphs[i]}_${galois_algos[algo]}.log
        		done
    		done
	done
done
