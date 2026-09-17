#!/bin/bash

input_dir=../../inputs_galois
exec_dir=./build/lonestar/analytics/cpu/sssp

graphs=(USAr LJ TW HYPERH)
obim_deltas=(14 0 0 1)      
pmod_deltas=(14 0 0 1)   
startnodes=(1 1 1 2)

galois_algos=(deltaStepOBIM deltaStepPMOD)

for t in {48,1}
do
	for algo in ${!galois_algos[@]}
	do 
    		for i in {0..3}
    		do
        		echo "Running SSSP ${galois_algos[algo]} on ${graphs[i]}"
        		echo "Results" > results_sssp_${t}_${graphs[i]}_${galois_algos[algo]}.log
        		declare -i delta=1
        		if [ ${galois_algos[algo]} = "deltaStepOBIM" ]; then
            			delta=${obim_deltas[i]}
        		else
            			delta=${pmod_deltas[i]}
        		fi
        		for j in {1..10}
        		do
            			echo $j
            			${exec_dir}/sssp-cpu -startNode ${startnodes[i]} -delta $delta -t $t -algo=${galois_algos[algo]} ${input_dir}/${graphs[i]}.gr >> results_sssp_${t}_${graphs[i]}_${galois_algos[algo]}.log
        		done
    		done
	done
done
