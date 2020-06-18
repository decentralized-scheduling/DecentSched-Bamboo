cd ../
#rm outputs/stats.json
cp -r config-tpcc-std.h config.h

fname="tpcc-pf"

## algorithm
alg=BAMBOO
latch=LH_MCSLOCK
# [WW]
ww_starv_free="false"
# [BAMBOO]
dynamic="true"
retire="true"
cs_pf="true"
opt_raw="true"
max_waiter=0

## workload
wl="TPCC"
wh=2
perc=0.5 # payment percentage
user_abort="true"
com="false"
com_latch="false"

#other
threads=8
profile="true"
cnt=100000
penalty=50000
chain="true"

for numa in true #false
do
for latch in LH_MCSLOCK LH_SPINLOCK
do
for i in 0 1 2 3 4
do
for user_abort in true false
do
for alg in BAMBOO WOUND_WAIT
do
for wh in 1 2 4 8 16
do
for threads in 1 2 4 8 16 32
do

if [ $alg == "BAMBOO" ]; then

if [ $threads -eq 32 ]; then

for max_waiter in 0 4 16
do
timeout 100 python test.py UNSET_NUMA=${numa} CC_ALG=$alg LATCH=${latch} WW_STARV_FREE=${ww_starv_free} DYNAMIC_TS=$dynamic RETIRE_ON=$retire DEBUG_CS_PROFILING=${cs_pf} BB_OPT_RAW=${opt_raw} BB_OPT_MAX_WAITER=${max_waiter} WORKLOAD=${wl} NUM_WH=${wh} PERC_PAYMENT=$perc TPCC_USER_ABORT=${user_abort} COMMUTATIVE_OPS=$com COMMUTATIVE_LATCH=${com_latch} THREAD_CNT=$threads DEBUG_PROFILING=${profile} MAX_TXN_PER_PART=$cnt ABORT_PENALTY=$penalty DEBUG_ABORT_LENGTH=${chain}
done

elif [ $threads -eq 16 ]; then

for max_waiter in 0 4 12
do
timeout 100 python test.py UNSET_NUMA=${numa} CC_ALG=$alg LATCH=${latch} WW_STARV_FREE=${ww_starv_free} DYNAMIC_TS=$dynamic RETIRE_ON=$retire DEBUG_CS_PROFILING=${cs_pf} BB_OPT_RAW=${opt_raw} BB_OPT_MAX_WAITER=${max_waiter} WORKLOAD=${wl} NUM_WH=${wh} PERC_PAYMENT=$perc TPCC_USER_ABORT=${user_abort} COMMUTATIVE_OPS=$com COMMUTATIVE_LATCH=${com_latch} THREAD_CNT=$threads DEBUG_PROFILING=${profile} MAX_TXN_PER_PART=$cnt ABORT_PENALTY=$penalty DEBUG_ABORT_LENGTH=${chain}
done

else 

max_waiter = 0
timeout 100 python test.py UNSET_NUMA=${numa} CC_ALG=$alg LATCH=${latch} WW_STARV_FREE=${ww_starv_free} DYNAMIC_TS=$dynamic RETIRE_ON=$retire DEBUG_CS_PROFILING=${cs_pf} BB_OPT_RAW=${opt_raw} BB_OPT_MAX_WAITER=${max_waiter} WORKLOAD=${wl} NUM_WH=${wh} PERC_PAYMENT=$perc TPCC_USER_ABORT=${user_abort} COMMUTATIVE_OPS=$com COMMUTATIVE_LATCH=${com_latch} THREAD_CNT=$threads DEBUG_PROFILING=${profile} MAX_TXN_PER_PART=$cnt ABORT_PENALTY=$penalty DEBUG_ABORT_LENGTH=${chain}

fi

else

max_waiter = 0
timeout 100 python test.py UNSET_NUMA=${numa} CC_ALG=$alg LATCH=${latch} WW_STARV_FREE=${ww_starv_free} DYNAMIC_TS=$dynamic RETIRE_ON=$retire DEBUG_CS_PROFILING=${cs_pf} BB_OPT_RAW=${opt_raw} BB_OPT_MAX_WAITER=${max_waiter} WORKLOAD=${wl} NUM_WH=${wh} PERC_PAYMENT=$perc TPCC_USER_ABORT=${user_abort} COMMUTATIVE_OPS=$com COMMUTATIVE_LATCH=${com_latch} THREAD_CNT=$threads DEBUG_PROFILING=${profile} MAX_TXN_PER_PART=$cnt ABORT_PENALTY=$penalty DEBUG_ABORT_LENGTH=${chain}

fi
done
done
done
done
done
done
done

#

cd outputs/
python3 collect_stats.py
mv stats.csv ${fname}.csv
mv stats.json ${fname}.json
cd ..

cd experiments/
python3 send_email.py ${fname}
