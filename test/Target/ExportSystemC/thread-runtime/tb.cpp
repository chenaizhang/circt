#include "timed_thread.hpp"

#include <iostream>

int sc_main(int argc, char **argv) {
  sc_signal<bool> done;
  delayed_process dut("dut");
  dut.done(done);

  sc_start(500, SC_PS);
  if (done.read()) {
    std::cerr << "thread completed before its delay\n";
    return 1;
  }
  sc_start(1, SC_NS);
  if (!done.read()) {
    std::cerr << "thread did not resume after its delay\n";
    return 1;
  }

  std::cout << "SYSTEMC_THREAD_RUNTIME_OK\n";
  return 0;
}
