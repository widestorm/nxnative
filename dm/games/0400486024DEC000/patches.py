# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO
# build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches")
from nso_asm import ADRP, B, STP_PRE, LDP_POST
# Frame step (2026-11-06). The game steps its world by its time object ([main+0x4e94b20], 0x1f0 bytes): main+0x3b4e300(time, step) sets +0x10 step, +0x14 1 / step, +0x18 step / 60 s,
# +0x1c 60 / step, and every channel ({step, 1 / step, seconds, rate} at +0x0, +0x20 ... +0x110) from step * [+0x1c0] * its modifier. The frame update main+0x6e6be0 calls it twice
# a frame (main+0x6e6dd8, +0x6e70f8) with s8 = (30 fps mode at [setting+0x24f] ? 30.0 : 60.0) / [setting+0x2c8], 60 / 60 = 1.0, whatever the frames really took, so below 60 fps
# the game ran slow (probes: 1.0 and 1/60 at 30-39 fps and at a held 30; the host writing the object's fields lost to this setter every frame, and Link stayed at half speed).
# The stub multiplies s8 by the host's step in vsyncs (state +0x20: 1 at 60, 2 at 30, more after a late frame), so the setter gets the time the frame is on screen for;
# 0 (not decided yet, or fps_target 0) keeps the game's value. x16 / x17 / d16 are saved, no flags change. Checked 2026-11-06 on an M4 at Frame rate 30: the update's step 2 and
# 1/30 s at 30 frames a second (OBS_PROBE gates at main+0x508c34 / +0x508c4c, removed since), Link and the world at their real speed
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
    {'what': "frame step: the time object's step times the host's vsyncs (v131072 main+0x6e6cc4)", 'now': 'build', 'goal': 'engine', 'purpose': 'frame rate',
     'why': "Nintendo EPD's engine (Tears of the Kingdom's file layout) may share the time object and its setter with other games", 'blocked': 'one game of this engine version here'},
]
