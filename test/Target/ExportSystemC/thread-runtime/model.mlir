emitc.include <"systemc.h">

systemc.module @delayed_process(%done: !systemc.out<i1>) {
  systemc.ctor {
    systemc.thread %run
  }
  %run = systemc.func {
    %false = hw.constant false
    systemc.signal.write %done, %false : !systemc.out<i1>
    systemc.wait_time 1000000
    %true = hw.constant true
    systemc.signal.write %done, %true : !systemc.out<i1>
  }
}
