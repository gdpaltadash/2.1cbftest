# Click Between Frames (GD 2.113)

Port of syzzi's Click Between Frames to **GD 2.113** and **Geode v1.3.10**.

GD 2.1 delivers every input at the start of the frame, so the exact moment of a click is lost. This mod timestamps jump inputs as the OS reports them, and splits the frame in two at that moment so the click lands where it really happened.

# Notes

- Windows only (32-bit GD 2.113). Under Wine the keyboard timestamps may not arrive; the mod then behaves like vanilla.
- Frames without a click run exactly like vanilla. Frames with a click run as two updates, so physics are slightly different from vanilla 2.1.
- "Safe Mode" blocks saving progress. Rated levels are never marked valid for leaderboards while CBF is active.
