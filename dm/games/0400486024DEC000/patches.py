# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO
# build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches")
_obs_probe = 27   # dm/host/dm.h; the upper bits: register | (offset / 4) << 5 | slot << 17 (dm/host/hle_obs_probe.c)
def probe(reg, off, slot): return _obs_probe | (reg | (off // 4) << 5 | slot << 17) << 8

# Diagnostic, frame clock (2026-11-06): held at 30 fps by DM_PRESENT_INTERVAL_MIN=2 the game runs at half speed, so it steps its world by a per-frame amount. Many systems take
# [ctx+0x3b40] * (1/60 at main+0x3e4066c); +0x3b40 is written once (main+0x32de7c) from the step argument of main+0x32dac0, called from main+0x79920 with [system+0xa68], which the
# virtual update at main+0x508c10 (vtable slot main+0x4c644d8) copies from its argument (+4). The same function multiplies a time at [*(main+0x4e94b20)]+0x18 by 60.0 into main+0x4d4454c.
# Both reads get a probe: DM_OBS_PROBE=1 logs them every 2 s, to compare a run at the game's own rate with one held at 30
OBS = {
  'e0f3ccfaf50065ae4ea01eb5aeb13b1a': [
    (0x508c34, probe(1, 0x4, 0), 'bd400420', 'probe slot 0: ldr s0,[x1,#4], the step the update copies into its system object (+0xa68)'),
    (0x508c4c, probe(8, 0x18, 1), 'bd401900', 'probe slot 1: ldr s0,[x8,#0x18], the time the update multiplies by 60.0'),
  ],
}

BINDING = [
    {'what': 'frame clock probes at main+0x508c34 and main+0x508c4c (diagnostic, v131072 main)', 'now': 'build', 'goal': 'build', 'purpose': 'frame rate',
     'why': 'read-only gates at fixed offsets of this build, for one measurement; they go once the frame step is understood'},
]
