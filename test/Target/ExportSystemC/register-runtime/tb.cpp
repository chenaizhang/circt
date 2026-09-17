#include "register_models.hpp"

#include <iostream>

int sc_main(int argc, char **argv) {
  sc_signal<bool> clk;
  sc_signal<bool> reset;
  sc_signal<bool> enable;
  sc_signal<sc_uint<8>> next;
  sc_signal<sc_uint<8>> value;
  sc_signal<bool> asyncReset;
  sc_signal<sc_uint<8>> asyncNext;
  sc_signal<sc_uint<8>> asyncValue;

  counter dut("dut");
  dut.clk(clk);
  dut.reset(reset);
  dut.enable(enable);
  dut.next(next);
  dut.value(value);

  async_register asyncDut("asyncDut");
  asyncDut.clk(clk);
  asyncDut.reset(asyncReset);
  asyncDut.next(asyncNext);
  asyncDut.value(asyncValue);

  auto pulse = [&] {
    clk.write(true);
    sc_start(1, SC_NS);
    clk.write(false);
    sc_start(1, SC_NS);
  };

  clk.write(false);
  reset.write(true);
  enable.write(false);
  next.write(0xff);
  asyncReset.write(false);
  asyncNext.write(0x56);
  sc_start(SC_ZERO_TIME);
  pulse();
  if (value.read() != 0) {
    std::cerr << "synchronous reset failed: " << value.read() << "\n";
    return 1;
  }

  reset.write(false);
  enable.write(true);
  next.write(0x12);
  pulse();
  if (value.read() != 0x12) {
    std::cerr << "enabled register update failed: " << value.read() << "\n";
    return 1;
  }

  enable.write(false);
  next.write(0x34);
  pulse();
  if (value.read() != 0x12) {
    std::cerr << "register hold failed: " << value.read() << "\n";
    return 1;
  }

  enable.write(true);
  pulse();
  if (value.read() != 0x34) {
    std::cerr << "second register update failed: " << value.read() << "\n";
    return 1;
  }

  if (asyncValue.read() != 0x56) {
    std::cerr << "asynchronous register clock update failed: "
              << asyncValue.read() << "\n";
    return 1;
  }
  asyncReset.write(true);
  sc_start(1, SC_NS);
  if (asyncValue.read() != 0) {
    std::cerr << "asynchronous reset failed without a clock edge: "
              << asyncValue.read() << "\n";
    return 1;
  }
  asyncReset.write(false);
  asyncNext.write(0x78);
  pulse();
  if (asyncValue.read() != 0x78) {
    std::cerr << "asynchronous register recovery failed: "
              << asyncValue.read() << "\n";
    return 1;
  }

  std::cout << "SYSTEMC_REGISTER_RUNTIME_OK\n";
  return 0;
}
