#!/bin/bash

input_dir=../../inputs_galois
exec_dir=./build/lonestar/analytics/cpu/sssp

graphs=(USAr LJ TW HYPERH)
bucket_deltas=(11 4 3 3)
bucket_batch1=(64 128 256 256)
bucket_batch2=(8 128 128 256)

heap_batch1=(2 128 256 256)
heap_batch2=(32 128 128 256)

stickiness=(64 1 1 1)

startnode=(1 1 1 2)

# MQBucket
for i in {0..3}
do
        echo "Results" > results_sssp_48_${graphs[i]}_MQBucket.log
	for j in {1...10}
	do
		${exec_dir}/sssp-cpu -startNode ${startnode[i]} -delta ${bucket_deltas[i]} -threads 48 -queues 96 -buckets 64 -batch1 ${bucket_batch1[i]} -batch2 ${bucket_batch2[i]} -prefetch 1 -stick ${stickiness[i]} -algo=MQBucket ${input_dir}/${graphs[i]}.gr >> results_sssp_48_${graphs[i]}_MQBucket.log
	done
done

for i in {0..3}
do
        echo "Results" > results_sssp_1_${graphs[i]}_MQBucket.log
        for j in {1...10}
        do
                ${exec_dir}/sssp-cpu -startNode ${startnode[i]} -delta ${bucket_deltas[i]} -threads 1 -queues 4 -buckets 64 -batch1 ${bucket_batch1[i]} -batch2 ${bucket_batch2[i]} -prefetch 1 -stick 1 -algo=MQBucket ${input_dir}/${graphs[i]}.gr >> results_sssp_1_${graphs[i]}_MQBucket.log
        done
done


# MQ
for i in {0..3}
do
        echo "Results" > results_sssp_48_${graphs[i]}_MQ.log
        for j in {1...10}
        do
                ${exec_dir}/sssp-cpu -startNode ${startnode[i]} -threads 48 -queues 96 -batch1 ${heap_batch1[i]} -batch2 ${heap_batch2[i]} -prefetch 1 -stick ${stickiness[i]} -algo=MQ ${input_dir}/${graphs[i]}.gr >> results_sssp_48_${graphs[i]}_MQ.log
        done
done

for i in {0..3}
do
        echo "Results" > results_sssp_1_${graphs[i]}_MQ.log
        for j in {1...10}
        do
        	 ${exec_dir}/sssp-cpu -startNode ${startnode[i]} -threads 1 -queues 4 -batch1 ${heap_batch1[i]} -batch2 ${heap_batch2[i]} -prefetch 1 -stick 1 -algo=MQ ${input_dir}/${graphs[i]}.gr >> results_sssp_1_${graphs[i]}_MQ.log
	done
done

# MQPlain
for i in {0..3}
do
        echo "Results" > results_sssp_48_${graphs[i]}_MQPlain.log
        for j in {1...10}
        do
                ${exec_dir}/sssp-cpu -startNode ${startnode[i]} -threads 48 -queues 192 -batch1 1 -batch2 1 -prefetch 0 -stick 1 -algo=MQ ${input_dir}/${graphs[i]}.gr >> results_sssp_48_${graphs[i]}_MQPlain.log
        done
done

for i in {0..3}
do
        echo "Results" > results_sssp_1_${graphs[i]}_MQPlain.log
        for j in {1...10}
        do
        	${exec_dir}/sssp-cpu -startNode ${startnode[i]} -threads 1 -queues 4 -batch1 1 -batch2 1 -prefetch 0 -stick 1 -algo=MQ ${input_dir}/${graphs[i]}.gr >> results_sssp_1_${graphs[i]}_MQPlain.log
	done
done
