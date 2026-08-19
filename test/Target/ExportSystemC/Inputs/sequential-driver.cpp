#include "sequential.cpp"

#include <iostream>

int sc_main(int argc, char **argv) {
  sc_clock clk("clk", 10, SC_NS);
  sc_signal<bool> reset;
  sc_signal<bool> enable;
  sc_signal<sc_uint<8>> next;
  sc_signal<sc_uint<8>> value;

  counter dut("dut");
  dut.clk(clk);
  dut.reset(reset);
  dut.enable(enable);
  dut.next(next);
  dut.value(value);

  reset.write(true);
  enable.write(true);
  next.write(7);
  sc_start(1, SC_NS);
  if (value.read() != 0) {
    std::cerr << "reset failed: " << value.read() << "\n";
    return 1;
  }

  reset.write(false);
  sc_start(10, SC_NS);
  if (value.read() != 7) {
    std::cerr << "capture failed: " << value.read() << "\n";
    return 1;
  }

  enable.write(false);
  next.write(9);
  sc_start(10, SC_NS);
  if (value.read() != 7) {
    std::cerr << "enable hold failed: " << value.read() << "\n";
    return 1;
  }

  enable.write(true);
  sc_start(10, SC_NS);
  if (value.read() != 9) {
    std::cerr << "second capture failed: " << value.read() << "\n";
    return 1;
  }

  std::cout << "SEQUENTIAL_RUNTIME_OK\n";
  return 0;
}
