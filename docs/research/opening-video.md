# The opening video of the first-kit title is black

An open bug. The Frostbite title of `first-kit-title.md` plays a video when it
starts. Here the screen stays black until a button skips it; after that the
loading screen and the main menu draw as they should.

## What is seen

- The title uploads three planes per frame with `UpdateSubresource`. They
  hold 0x10 for luma and 0x80 for chroma, which is black: the textures are
  never given a decoded frame (`XV_D3D_SEQ=<frame>` shows the bytes).
- The back buffer average at frame 900 is 1.1, 0.0, 1.1.
- Nothing faults and no call is reported as unimplemented.

## What was measured

With breakpoints in the title (`XV_BREAK`, `XV_BREAK_LIMIT`), one minute of
running each time:

- The decoder is VP6, built into the title (its jobs are named `vp6_Deco...`
  and `vp6_mbRo...`, the stream's magic is `VP60`). It splits a frame into
  jobs for the engine's job system. A last job, which depends on the others,
  hands the frame to the player (`image+0xf79860`).
- The decode thread queued 24 and 32 frames in two runs. The last job ran
  exactly 8 times in both, for the first eight frames queued, in order.
- The player took a decoded frame 8 times and never gave one back
  (`image+0xd7e4bb` 8 hits, the release at `image+0xd87090` 8 hits from the
  player's start only).
- The player's test for showing a frame, its number against the number the
  clock gives (`image+0xd830fb`), ran 2402 times and failed every time.

So frames are decoded. The eight the player can hold fill up, nothing takes
them, and the decoder waits for a free frame context, which is where
`XV_THREAD_DUMP` finds its thread.

## What is inferred, not verified

Read from the disassembly, not confirmed by running:

- The clock (`image+0xd83720`, reached through the timer object) asks an
  audio object for how much it has played and turns that into a frame
  number. If so, the video follows its sound track.
- The sound track would be XMA, which the console decodes in its audio
  processor. Nothing decodes XMA here: the audio processor acknowledges
  commands and ticks audio frames, and its contexts never produce output.
  A voice that never gets data never advances, and the clock stays at zero.

The first thing to do next is to check both: a breakpoint after the clock's
call to see what it returns, and one on the voice's position.

## Ruled out

- **The job system.** Workers are idle because there is nothing to run, not
  because jobs are stuck.
- **Processor count and affinity.** Reporting six or eight processors, and
  pinning the process to six or to one, changed nothing. The title pins its
  workers with masks 0x1 to 0x20 and 0x80 (`XV_THREAD_TRACE=1`).
- **Completed audio commands.** In this kit a completed command's id is 12
  bytes into the message; that was wrong and is fixed, and the video did not
  change.
- **The Direct3D side.** The uploads arrive and are drawn; they are black
  at the source.

## What was tried and did not work

Running the XMA contexts (consume the input, write silence to the output
ring) changed nothing, and was taken out. It read each context's fields from
the data of the update command, which is empty in this kit: the title keeps
a pending copy of a context and applies the fields itself when the command
completes (`image+0x119e240`). During the video only context 0 is enabled
and disabled, with one update between (`XV_ACP_TRACE=1`). A second attempt
was written and never applied, so it tells nothing.

## What a fix needs

If the inference holds: the audio processor's XMA contexts have to run.
Read the fields from the context in the array the runtime allocated
(`AcpHalAllocateShapeContexts`), consume input at the rate of the stream,
write output to the context's ring and move its offsets, so that the title's
mixer sees data and its voice advances. Silence is enough for the picture;
sound needs an XMA decoder.

The layout of the context and the commands are in the console's audio
headers. Two things to settle first: whether the title's
`ApuMapVirtualAddress` has the form the runtime implements (the later header
returns the address instead of taking a place for it), and what the title
reads back from a context to know that output is there.
