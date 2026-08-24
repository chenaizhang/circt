# SystemC 后端分阶段落地说明

本文记录当前 fork 中 SystemC 后端的实现边界和验证顺序。主路径只依赖 CIRCT 的 HW、Comb、Seq 和 SystemC dialect；需要运行真实 RTL 时，Verilator 只作为外部参考模型或临时黑盒。

## 统一出口

所有路径最终都经过 `ExportSystemC`。因此，结构转换、组合逻辑、时序逻辑和诊断信息不能分别绕过 emitter。当前出口已经注册 Sim dialect，并支持：

- `sim.fmt.literal`：发射为 C++ 字符串字面量；
- `sim.proc.print`：发射为 `SC_REPORT_ERROR`；
- `sim.print`：按条件发射诊断语句；
- `systemc.signal.read` 读取 `sc_out`：SystemC 的 `sc_out` 继承自 `sc_inout`，允许反馈读取。

## 转换顺序

1. HW 阶段生成 `SC_MODULE`、端口、信号、子模块声明和绑定关系。
2. Comb 阶段生成算术、位运算、比较、MUX、拼接和抽取表达式。
3. Seq 阶段生成寄存器、时钟边沿、复位和 next-state。
4. 每个阶段都重新经过 `ExportSystemC`，并用编译器检查生成的 C++。

LLHD 定时过程降级到 HW/Comb/Seq 时，会把图区域中的操作移动到 SSA block。移动完成后必须尽可能恢复拓扑顺序；否则后续聚合展开会报告误导性的 `comb.concat` 类型错误或支配关系错误。`llhd-lower-timed-processes` 现在在模块体上执行稳定的拓扑排序；若剩余环是寄存器的 next-state 反馈，则保留该语义环并发出 warning，交给 Seq/SystemC lowering 处理，而不是把它误报成 pass 失败。

## 验证命令

最小出口测试：

```bash
circt-translate test/Target/ExportSystemC/sim.mlir --export-systemc
```

真实设计验证必须在 x86 服务器运行，至少保存：

- HW/Comb/Seq 各阶段 MLIR；
- 每阶段的 stderr；
- 生成的 SystemC MLIR/C++；
- C++ 编译命令和结果；
- 首个未支持 operation。

只有在生成的 SystemC 通过语法、编译和运行检查后，才能进入 Verilator 黑盒替换或逐模块差分。

## 当前 x86 实测结果

截至 2026-08-24，服务器上的实测结果如下：

| 用例 | 结果 |
|---|---|
| `sim.mlir`（Sim literal/print、`sc_out.read()`） | 生成 C++ 通过 |
| HW→SystemC `structure.mlir` | 转换通过 |
| HW→SystemC `sequential.mlir` | 转换、ExportSystemC 和 C++ 语法检查通过 |
| HW→SystemC `aggregate.mlir` | 转换、ExportSystemC 和 C++ 语法检查通过 |
| 真实设计 HW 结构阶段 | 生成、导出、编译和运行探针通过 |
| 真实设计 LLHD 定时过程阶段 | lowering 通过；对寄存器反馈环发出 warning |
| 真实设计完整 HW/Comb/Seq | 仍未闭合 |

真实设计的当前首错已从 LLHD lowering 推进到 `HW→SystemC` 输入校验：部分 `comb.concat` 的操作数宽度与声明结果不一致，同时存在由顺序反馈造成的支配关系问题。另有一条独立的前端问题：全源集合仍引用未声明的 `tDSC_SAMPLE`/`kDSC_SAMPLE_INIT`，可达设计前端不受影响。上述问题属于真实 RTL/降级 IR 的下一层修复，不是 SystemC 库或编译器安装问题。

## 尚未闭合的部分

`systemc.interop.verilated` 的持久化对象生命周期和端口读写仍需要专门的 Interop→EmitC/SystemC lowering；不能把“生成了 interop dialect”当作已经生成可编译 C++。在该路径闭合前，Verilator 模型继续作为独立参考/黑盒使用。
