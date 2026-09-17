#include "memory_models.hpp"

#include <iostream>

int sc_main(int argc, char **argv) {
  sc_clock clk("clk", 10, SC_NS);
  sc_signal<bool> writeEnable;
  sc_signal<sc_uint<4>> address;
  sc_signal<sc_uint<8>> writeData;
  sc_signal<sc_uint<8>> readData;

  memory dut("dut");
  dut.clk(clk);
  dut.writeEnable(writeEnable);
  dut.address(address);
  dut.writeData(writeData);
  dut.readData(readData);

  writeEnable.write(false);
  address.write(3);
  writeData.write(0x5a);
  sc_start(1, SC_NS);

  writeEnable.write(true);
  sc_start(10, SC_NS);
  writeEnable.write(false);
  // The source memory declares read-under-write as undefined. Avoid relying
  // on a particular collision result and retrigger the asynchronous read after
  // the write has committed.
  address.write(4);
  sc_start(1, SC_NS);
  address.write(3);
  sc_start(1, SC_NS);

  if (readData.read() != 0x5a) {
    std::cerr << "memory readback failed: " << readData.read() << "\n";
    return 1;
  }

  std::cout << "SYSTEMC_MEMORY_RUNTIME_OK\n";
  return 0;
}
