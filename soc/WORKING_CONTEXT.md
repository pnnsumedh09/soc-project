# Golden GEMV context

Read `soc/README.md` for the selected source paths, hardware hashes, build
defaults, validation status and deployment prerequisites.

The selected hardware is the board-tested sequential 100 MHz HLS export.
Do not include the handwritten FIFO profiling RTL or its bitstream.
Do not replace it with row8 until that candidate completes RTL co-simulation,
implementation, timing checks and board validation on a separate branch.

The clean checkout includes narrowly scoped source cleanup and new ARM/Linux
build/backend defaults. Those changes passed cross-build and short board CLI/server checks; see
`soc/validation/golden.json`. Full course benchmark and grader checks remain pending. Preserve the original experimental checkout and all its artifacts.

Only read files required by the current task. Never stage the entire repository
blindly. Do not push to the grading branch without explicit user instruction.
