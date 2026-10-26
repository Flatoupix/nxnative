# Good Job! (1.0.0): keep Unity's frame pacing on the Mac at the profile target.
#
# Unity 2018.4's managed wrappers receive the game's Switch values after boot:
# Application.targetFrameRate is set to 30 and QualitySettings.vSyncCount to 2.
# The host presents once per display tick, but those Unity values still make the
# player loop alternate missed 60 Hz ticks.  The two wrapper arguments below are
# read dynamically from the host's frame-state page, so the launcher's 30/60 fps
# setting remains coherent (60/1 or 30/2).  Game time in Good Job! is time-based.
from nso_asm import ADRP, B
def _dynamic_fps_arg(ctx, off, original, divisor, what):
  """Replace mov w19,w0 with the profile target (or 60 / target) from ctx.state."""
  stub = ctx.here()
  ctx.site(off, original)
  ctx.put(off, [B(off, stub)], f'{what}: read the launcher fps target')
  ctx.emit(ADRP(16, ctx.here(), ctx.state))
  ctx.emit(0xbd400213)  # ldr s19,[x16]
  ctx.emit(0x1e380273)  # fcvtzs w19,s19
  if divisor:
    ctx.emit(0x52800791)  # mov w17,#60
    ctx.emit(0x1ad30a33)  # udiv w19,w17,w19
  ctx.emit(B(ctx.here(), off + 4))


def _frame_step(ctx):
  _dynamic_fps_arg(ctx, 0x6014b8, 'f303002a', False, 'Unity Application.set_targetFrameRate')
  _dynamic_fps_arg(ctx, 0x737308, 'f303002a', True, 'Unity QualitySettings.set_vSyncCount')


FRAME_STEP = {
  '951d09eece122a479e355a3da8c85e9b': _frame_step,
}
