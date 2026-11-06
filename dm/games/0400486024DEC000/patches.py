# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO
# build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches")
from nso_asm import ADRP, B, STP_PRE, LDP_POST
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

# Frame step (2026-11-06). The game steps its world by its time object ([main+0x4e94b20], 0x1f0 bytes): main+0x3b4e300(time, step) sets +0x10 step, +0x14 1 / step, +0x18 step / 60 s,
# +0x1c 60 / step, and every channel ({step, 1 / step, seconds, rate} at +0x0, +0x20 ... +0x110) from step * [+0x1c0] * its modifier. The frame update main+0x6e6be0 calls it twice
# a frame (main+0x6e6dd8, +0x6e70f8) with s8 = (30 fps mode at [setting+0x24f] ? 30.0 : 60.0) / [setting+0x2c8], 60 / 60 = 1.0, whatever the frames really took, so below 60 fps
# the game ran slow (probes: 1.0 and 1/60 at 30-39 fps and at a held 30; the host writing the object's fields lost to this setter every frame, and Link stayed at half speed).
# The stub multiplies s8 by the host's step in vsyncs (state +0x20: 1 at 60, 2 at 30, more after a late frame), so the setter gets the time the frame is on screen for;
# 0 (not decided yet, or fps_target 0) keeps the game's value. x16 / x17 / d16 are saved, no flags change
def frame_step(ctx):
    ctx.site(0x6e6cc4, '2818201e')   # fdiv s8, s1, s0
    start = ctx.here()
    ctx.emit(0x1e201828)                                   # fdiv s8, s1, s0       the game's step
    ctx.emit(STP_PRE(16, 17, 31, -16))
    ctx.emit(ADRP(16, ctx.here(), ctx.state))
    for w in (0xb9402211,   # ldr w17, [x16, #0x20]  host step in vsyncs
              0x340000b1,   # cbz w17, out (+5)     not decided: the game's step
              0xfc1f0ff0,   # str d16, [sp, #-0x10]!
              0x1e230230,   # ucvtf s16, w17
              0x1e300908,   # fmul s8, s8, s16      step * vsyncs
              0xfc4107f0):  # ldr d16, [sp], #0x10
        ctx.emit(w)
    ctx.emit(LDP_POST(16, 17, 31, 16))                     # out:
    ctx.emit(B(ctx.here(), 0x6e6cc8))
    ctx.put(0x6e6cc4, [B(0x6e6cc4, start)], "frame step from the host in whole vsyncs: the time object's step times the vsyncs the frame is on screen for (state +0x20; 0 = the game's step)")


FRAME_STEP = {'e0f3ccfaf50065ae4ea01eb5aeb13b1a': frame_step}

BINDING = [
    {'what': 'frame clock probes at main+0x508c34 and main+0x508c4c (diagnostic, v131072 main)', 'now': 'build', 'goal': 'build', 'purpose': 'frame rate',
     'why': 'read-only gates at fixed offsets of this build, for one measurement; they go once the frame step is understood'},
    {'what': "frame step: the time object's step times the host's vsyncs (v131072 main+0x6e6cc4)", 'now': 'build', 'goal': 'engine', 'purpose': 'frame rate',
     'why': "Nintendo EPD's engine (Tears of the Kingdom's file layout) may share the time object and its setter with other games", 'blocked': 'one game of this engine version here'},
]
