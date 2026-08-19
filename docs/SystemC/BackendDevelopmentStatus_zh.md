# SystemC 后端补全与验证说明

本文记录本分支对 CIRCT SystemC 后端的补全范围、设计边界和验证方法。该分支用于开发验证，当前不对应上游 PR。

## 目标

将 SystemVerilog 前端产生的 Core IR 分阶段转换为可编译的 SystemC：

1. 保留 HW 模块、端口、实例和模块间连线；
2. 发射常见 Comb 表达式；
3. 支持基础 Seq 寄存器、时钟使能和复位；
4. 消除不含时序控制的 SystemVerilog task 所产生的 LLHD coroutine；
5. 对暂不支持的 operation 给出明确错误，避免生成带有占位文本的伪成功代码。

## 本分支实现

### 无暂停 coroutine 内联

新增 `llhd-inline-suspend-free-coroutines` pass。它会内联至少包含一个 `llhd.return`，并且不包含 `llhd.wait`、`llhd.halt` 或嵌套 coroutine 调用的 `llhd.coroutine`。

该 pass 支持多基本块控制流：先建立源块到目标块的映射，再克隆操作和分支，并把所有 `llhd.return` 接到调用点后的 continuation block。它已接入 SystemVerilog 前端的 LLHD-to-Core pipeline，用于处理不含 timing control 的 task。

包含等待、暂停或嵌套 coroutine 调用的任务仍保持原样，不会被错误地当作普通函数内联。

### Comb C++ 发射

SystemC exporter 新增以下 Comb operation 的表达式发射：

- `comb.add`、`comb.sub`、`comb.mul`；
- 有符号和无符号除法、取模、右移，以及左移；
- `comb.and`、`comb.or`、`comb.xor`；
- 整数比较；
- `comb.mux`；
- `comb.concat`、`comb.extract`、`comb.replicate`、`comb.parity`。

表达式会显式使用 `sc_uint`、`sc_int`、`sc_biguint` 或 `sc_bigint` 保持 RTL 位宽截断和有符号运算语义。`systemc.convert` 同样按目标位宽发射，不再输出 `UNSUPPORTED OPERATION`。

### 基础 Seq lowering

`convert-hw-to-systemc` 现在能够处理：

- `seq.to_clock`、`seq.from_clock`；
- `seq.compreg`；
- `seq.compreg.ce`；
- `seq.firreg` 的同步复位和异步复位形式。

每个寄存器会生成一个模块内 `systemc.signal`。method 开始时读取 current-state，结束时根据正沿、enable 和 reset 计算 next-state 并写回。所有寄存器先读取旧值再统一写回，从而保持同一时钟边沿上的并行更新语义。

新增 `systemc.signal.posedge` operation，并由 exporter 发射为布尔 SystemC channel 的 `.posedge()` 查询。

当前明确不支持寄存器 initial/preset value；遇到这类语义时转换会失败并报告原因。

## 分阶段职责

| 阶段 | 当前职责 | 本分支状态 |
| --- | --- | --- |
| LLHD 预处理 | 消除无 timing control 的 task coroutine | 已实现 |
| HW | 模块、端口、实例、内部 signal 和端口绑定 | 使用现有转换，并修通进入条件 |
| Comb | 组合表达式到 SystemC C++ | 已补充常用 operation |
| Seq | 寄存器、enable、同步/异步 reset | 已补充基础形式 |
| Export | SystemC dialect 到 C++ | 已补充类型转换、组合表达式和正沿查询 |

## 验证方法

正式验证应在 x86 Linux 环境进行。以下命令假定已按 CIRCT 官方方式配置 `build` 目录：

```bash
cmake --build build --target circt-opt circt-translate -j4

build/bin/llvm-lit \
  test/Dialect/LLHD/Transforms/inline-suspend-free-coroutines.mlir \
  test/Conversion/HWToSystemC/sequential.mlir \
  test/Target/ExportSystemC/comb.mlir \
  test/Target/ExportSystemC/sequential.mlir
```

单独检查 Seq 转换和 C++ 发射：

```bash
build/bin/circt-opt --convert-hw-to-systemc \
  test/Conversion/HWToSystemC/sequential.mlir \
  -o build/sequential.systemc.mlir

build/bin/circt-translate --export-systemc \
  build/sequential.systemc.mlir \
  -o build/sequential.cpp

g++ -std=c++17 -fsyntax-only \
  $(pkg-config --cflags systemc) \
  build/sequential.cpp

g++ -std=c++17 -Ibuild \
  $(pkg-config --cflags systemc) \
  test/Target/ExportSystemC/Inputs/sequential-driver.cpp \
  $(pkg-config --libs systemc) \
  -o build/sequential-runtime

build/sequential-runtime
```

真实设计应按以下顺序运行，以便记录第一个仍不支持的 operation：

```text
SV frontend/Core IR
  -> inline suspend-free coroutine
  -> LLHD-to-Core cleanup
  -> HW aggregate cleanup
  -> convert-hw-to-systemc
  -> export-systemc
  -> C++ syntax/compile test
```

## x86 实测结果

本分支已在 x86-64 Linux 环境完成以下验证：

| 项目 | 结果 |
| --- | --- |
| `circt-opt`、`circt-translate` 增量构建 | 通过 |
| HW-to-SystemC、SystemC exporter、coroutine 和 SV 前端定向回归 | 10/10 通过 |
| 完整 `check-circt` 回归 | 通过：1625 项通过、5 项跳过、6 项预期失败 |
| 生成的组合逻辑 SystemC C++ 语法编译 | 通过 |
| 生成的时序逻辑 SystemC C++ 语法编译 | 通过 |
| reset、clock enable、保持和再次采样运行测试 | 通过，输出 `SEQUENTIAL_RUNTIME_OK` |
| 真实 DSC Core IR 中无暂停 task 展开 | 1 个 coroutine、30 个调用全部消除 |

其中 SV 前端回归直接以包含 automatic task 和条件分支的 SystemVerilog 为输入，`circt-verilog --ir-hw` 的结果中不再残留 `llhd.coroutine` 或 `llhd.call_coroutine`。

真实 DSC Core IR 已把原先最先阻塞转换的 package task 消除。继续执行 LLHD、HW aggregate 和 HW-to-SystemC pipeline 后，当前最早的阻塞转移到 aggregate 表示：

```text
hw.bitcast: i216 -> !hw.array<18xi12>
```

运行 `hw-aggregate-to-comb` 和 `hw-convert-bitcasts` 可以在 `hw.bitcast` 与 `hw.array_create` 之间展开表示，但当前 SystemC type converter 尚未定义 `!hw.array`/`!hw.struct` 的 SystemC 表示。设计中还存在动态 `hw.array_slice`、`hw.struct_inject`，以及包含 `llhd.wait` 的 process。这些是全设计转换的下一组独立工作，不属于本次已经修复的 coroutine、常用 Comb emission 和基础 Seq lowering。

因此当前结论是：

- 普通标量 HW 模块、实例、端口和直连胶水结构可以生成 SystemC；
- 本分支新增的常用 Comb 与基础 Seq 可以生成、编译并运行；
- 真实 DSC 的无 timing-control task 已被清除；
- 真实 DSC 尚不能整体导出，下一阻塞是 aggregate type/operation，再之后是带 timing-control 的 LLHD process。

## 当前边界

本次补全不是“任意 SystemVerilog 均可直接转换”的承诺。以下内容仍需独立实现或继续 lowering：

- 带 `wait` 或其他 timing control 的 coroutine；
- inout/LLHD reference channel；
- memory 和复杂 aggregate port；
- initial/preset state；
- 四态值在 case/wildcard equality 中的完整语义；
- 超过当前 SystemC 整数类型覆盖范围的超宽运算。

因此，验证报告必须分别给出 HW、Comb、Seq 和 emission 的结果，不应再把所有失败笼统描述为“CIRCT 转换失败”。
