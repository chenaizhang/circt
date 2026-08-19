// RUN: circt-opt %s --llhd-inline-suspend-free-coroutines | FileCheck %s

module {
  llhd.coroutine private @choose_min(%lhs: i17, %rhs: i17,
                                    %result: !llhd.ref<i17>) {
    %delay = llhd.constant_time <0ns, 0d, 1e>
    %less = comb.icmp ult %lhs, %rhs : i17
    cf.cond_br %less, ^use_lhs, ^use_rhs
  ^use_lhs:
    llhd.drv %result, %lhs after %delay : i17
    cf.br ^done
  ^use_rhs:
    llhd.drv %result, %rhs after %delay : i17
    cf.br ^done
  ^done:
    llhd.return
  }

  hw.module @task_user(in %lhs : i17, in %rhs : i17, out result : i17) {
    %zero = hw.constant 0 : i17
    %result.signal = llhd.sig %zero : i17
    %result.value = llhd.prb %result.signal : i17
    llhd.combinational {
      llhd.call_coroutine @choose_min(%lhs, %rhs, %result.signal) : (i17, i17, !llhd.ref<i17>) -> ()
      llhd.yield
    }
    hw.output %result.value : i17
  }
}

// CHECK-NOT: llhd.coroutine
// CHECK-NOT: llhd.call_coroutine
// CHECK: llhd.combinational {
// CHECK: %[[LESS:.*]] = comb.icmp ult %lhs, %rhs : i17
// CHECK: cf.cond_br %[[LESS]]
// CHECK: llhd.drv %result.signal, %lhs after %{{.*}} : i17
// CHECK: llhd.drv %result.signal, %rhs after %{{.*}} : i17

// -----

module {
  llhd.coroutine private @wait_for_signal(%signal: !llhd.ref<i1>) {
    %value = llhd.prb %signal : i1
    llhd.wait (%value : i1), ^resume
  ^resume:
    llhd.return
  }

  llhd.coroutine private @timed_task(%signal: !llhd.ref<i1>) {
    llhd.call_coroutine @wait_for_signal(%signal) : (!llhd.ref<i1>) -> ()
    llhd.return
  }
}

// CHECK: llhd.coroutine private @wait_for_signal
// CHECK: llhd.call_coroutine @wait_for_signal
