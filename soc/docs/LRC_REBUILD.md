# Rebuild for board bring-up

The previous bitstream loaded on PYNQ, but the first control-register read did not return and USB xHCI failed. The cause is unresolved. The new builds are not a proven fix for that failure. An old active overlay or driver can leave AXI transactions outstanding during replacement; the presence of an old idle design alone does not identify the cause. Use a fresh board boot for the next reviewed deployment.

On LRC, from this checkout:

```bash
git pull --ff-only origin soc-gemv-accel
module load xilinx/2022
source /usr/local/packages/Xilinx_2022.2/Vivado/2022.2/settings64.sh
module load gcc/14
bash soc/tools/rebuild_lrc.sh software
bash soc/tools/rebuild_lrc.sh hls
bash soc/tools/rebuild_lrc.sh bd
bash soc/tools/rebuild_lrc.sh bitstream
```

Run each command only after the previous one succeeds. The HLS export includes C simulation, synthesis, RTL co-simulation and IP export. It refuses to reset an existing f100 project; preserve it by renaming that exact project directory if needed. The software build uses a separate `build-ultra96-bringup` directory and disables UI assets from the first configuration. Vivado rebuilds its f100 project. Preserve any old f100 Vivado project before running the BD stage if needed.

Outputs:

- `build-ultra96-bringup/bin/llama-cli`
- `build-ultra96-bringup/bin/llama-server`
- `soc/hw/out/gemv_f100_b64_o8.bit`, `.hwh`, `.xsa`
- `soc/hw/vivado/gemv_f100_b64_o8/clocks.rpt`
- `soc/hw/vivado/gemv_f100_b64_o8/timing_summary.rpt`
- `soc/hw/vivado/gemv_f100_b64_o8/utilization.rpt`
- Logs and source commit under `soc/docs/results/lrc_rebuild_100/`

Vivado now checks that the implemented PL0 clock is 10 ns before exporting the bundle. Failure to find that clock is a diagnostic failure, not authorization to deploy. The Linux DMA adapter now limits its pool to the implemented low 2 GiB DDR window. This address check does not resolve a control-read hang.

Return the final build log, clocks/timing/utilization reports and matching HWH for review before board loading. On the recovered board, collect `board_preflight.py` output without loading this overlay. We must check the PYNQ version, boot hardware configuration, actual PS clock/reset state and HWH control mapping. No DMA job or FPGA inference is authorized by a successful build alone.
