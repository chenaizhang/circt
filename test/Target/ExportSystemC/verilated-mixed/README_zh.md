# Verilator 叶子与 CIRCT SystemC 胶水端到端测试

该目录验证完整链路：

1. `Leaf.sv` 由 Verilator 生成 `VLeaf.h` 和静态库；
2. CIRCT 将 `hw.instance` 自动包装为 `systemc.interop.verilated`；
3. `HW → SystemC`、实例侧和容器侧 lowering 生成 `Top.h`；
4. SystemC 测试台链接 `VLeaf`，运行两组输入并检查输出。

在 x86 服务器上执行：

```bash
./run.sh
```

脚本会在临时构建目录中生成中间文件，不修改源目录。
