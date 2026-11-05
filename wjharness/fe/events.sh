# The front-end TMA event set (sourced). FE_EVENTS = fixed top-down group + instructions/cycles + seven groups of four
# general-purpose events (R458/s0b: more than four per group multiplexes on a hyperthreaded P-core).
C=${WJ_PMU_CORE:-cpu_core}   # Intel big-core named events: the hybrid PMU is cpu_core, a uniform part's is cpu
FE_T="{$C/slots/,$C/topdown-retiring/,$C/topdown-bad-spec/,$C/topdown-fe-bound/,$C/topdown-be-bound/,$C/topdown-heavy-ops/,$C/topdown-br-mispredict/,$C/topdown-fetch-lat/,$C/topdown-mem-bound/}"
FE_I="$C/instructions/,$C/cycles/"
FE_A="{$C/ICACHE_DATA.STALLS/,$C/ICACHE_TAG.STALLS/,$C/INT_MISC.CLEAR_RESTEER_CYCLES/,$C/INT_MISC.UNKNOWN_BRANCH_CYCLES/}"
FE_B="{$C/DSB2MITE_SWITCHES.PENALTY_CYCLES/,$C/DECODE.LCP/,$C/baclears.any/,$C/BR_INST_RETIRED.NEAR_TAKEN/}"
FE_C="{$C/IDQ.MITE_CYCLES_ANY/,$C/IDQ.MITE_CYCLES_OK/,$C/IDQ.DSB_CYCLES_ANY/,$C/IDQ.DSB_CYCLES_OK/}"
FE_D="{$C/IDQ.DSB_UOPS/,$C/IDQ.MITE_UOPS/,$C/IDQ.MS_UOPS/,$C/LSD.UOPS/}"
FE_E="{$C/UOPS_RETIRED.MS,cmask=1,edge/,$C/UOPS_RETIRED.SLOTS/,$C/UOPS_ISSUED.ANY/,$C/INT_MISC.UOP_DROPPING/}"
FE_F="{$C/ITLB_MISSES.WALK_ACTIVE/,$C/ITLB_MISSES.STLB_HIT/,$C/FRONTEND_RETIRED.L1I_MISS/,$C/BR_INST_RETIRED.ALL_BRANCHES/}"
FE_G="{$C/IDQ.MS_CYCLES_ANY/,$C/LSD.CYCLES_ACTIVE/,$C/LSD.CYCLES_OK/,$C/BR_MISP_RETIRED.ALL_BRANCHES/}"
# The top-down group and instructions/cycles are PINNED (:D): they are the denominators and count the whole window; the
# seven GP groups rotate around them (perf scales each).
FE_EVENTS="${FE_T}:D,{$FE_I}:D,$FE_A,$FE_B,$FE_C,$FE_D,$FE_E,$FE_F,$FE_G"
