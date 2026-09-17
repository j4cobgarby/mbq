#!/bin/bash

input_dir=../../inputs_galois
exec_dir=./build/lonestar/analytics/cpu/pagerank

graphs=(USA LJ TW HYPERH) 
obim_deltas=(21 20 20 20)
pmod_deltas=(21 20 20 19)
galois_algos=(OBIMPR PMODPR)

for algo in ${!galois_algos[@]}
do
	for i in {0..3}
	do
		echo "Results" > results_pr_48_${graphs[i]}_${galois_algos[algo]}.log
		declare -i delta=1
		if [ ${galois_algos[algo]} = "OBIMPR" ]; then
			delta=${obim_deltas[i]}
		else
            		delta=${pmod_deltas[i]}
		fi

		for j in {1..10}
		do
		${exec_dir}/pagerank-push-cpu -delta $delta -algo=${galois_algos[algo]} -t 48 ${input_dir}/${graphs[i]}.gr >> results_pr_48_${graphs[i]}_${galois_algos[algo]}.log
		done
	done
done

for algo in ${!galois_algos[@]}
do
        for i in {0..3}
        do
                echo "Results" > results_pr_1_${graphs[i]}_${galois_algos[algo]}.log
                declare -i delta=1
                if [ ${galois_algos[algo]} = "OBIMPR" ]; then
                        delta=${obim_deltas[i]}
                else
                        delta=${pmod_deltas[i]}
                fi

                for j in {1..10}
                do
                ${exec_dir}/pagerank-push-cpu -delta $delta -algo=${galois_algos[algo]} -t 1 ${input_dir}/${graphs[i]}.gr >> results_pr_1_${graphs[i]}_${galois_algos[algo]}.log
                done
        done
done
